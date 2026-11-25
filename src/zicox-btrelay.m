/*
 * zicox-btrelay - bridge local TCP ports to the CC4 over Bluetooth RFCOMM (SPP).
 *
 * On current macOS, opening /dev/cu.CC4_xxxx no longer brings up the
 * Bluetooth link, so this agent talks RFCOMM directly through IOBluetooth.
 * Bluetooth access is privacy-protected (TCC): the agent ships as
 * ZicoxBluetooth.app with NSBluetoothAlwaysUsageDescription and runs as a
 * user LaunchAgent, so macOS asks once for permission.
 *
 * Ports (127.0.0.1):
 *   9101  print port  - CUPS queue "socket://127.0.0.1:9101". With -s the printer
 *                       status is checked first and the job is refused (TCP reset,
 *                       CUPS retries) when paper is out or the cover is open.
 *   9102  control port - transparent, used by zicoxctl (status, self-test ...).
 *
 * One connection = one Bluetooth session: the link is opened for the job and
 * closed afterwards, so a phone can use the printer between jobs.
 *
 * Usage: zicox-btrelay [-a auto|name-prefix|xx:xx:xx:xx:xx:xx] [-c channel]
 *                      [-p port] [-P control-port] [-s] [-v]
 */

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include "zcio.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>

static int verbose = 0;

static void logmsg(const char *fmt, ...)
{
  char ts[32];
  time_t t = time(NULL);
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s zicox-btrelay: ", ts);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  fflush(stderr);
}

@interface ZCLink : NSObject <IOBluetoothRFCOMMChannelDelegate>
@property (strong) NSMutableData *rx;
@property BOOL closed;
@end

@implementation ZCLink
- (instancetype)init { if ((self = [super init])) _rx = [NSMutableData data]; return self; }
- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel *)c data:(void *)p length:(size_t)n { [self.rx appendBytes:p length:n]; }
- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel *)c { self.closed = YES; }
@end

static void spin(double seconds)
{
  [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                           beforeDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

static NSString *g_spec = @"auto";
static int g_channel = 0;

static IOBluetoothDevice *find_device(void)
{
  NSString *spec = g_spec;
  if (spec.length == 17 && ([spec containsString:@":"] || [spec containsString:@"-"]))
    return [IOBluetoothDevice deviceWithAddressString:spec];
  NSString *prefix = [spec isEqualToString:@"auto"] ? @"CC4" : spec;
  for (IOBluetoothDevice *d in [IOBluetoothDevice pairedDevices])
    if ([d.name hasPrefix:prefix]) return d;
  return nil;
}

static BluetoothRFCOMMChannelID find_channel(IOBluetoothDevice *dev)
{
  if (g_channel > 0) return (BluetoothRFCOMMChannelID)g_channel;
  IOBluetoothSDPServiceRecord *rec =
      [dev getServiceRecordForUUID:[IOBluetoothSDPUUID uuid16:kBluetoothSDPUUID16ServiceClassSerialPort]];
  BluetoothRFCOMMChannelID cid = 0;
  if (rec && [rec getRFCOMMChannelID:&cid] == kIOReturnSuccess && cid) return cid;
  return 1;
}

static void abort_client(int cfd)
{
  struct linger lg = { 1, 0 };
  setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
  close(cfd);
}

static BOOL bt_write(IOBluetoothRFCOMMChannel *ch, const unsigned char *p, size_t n)
{
  BluetoothRFCOMMMTU mtu = [ch getMTU];
  if (mtu == 0) mtu = 127;
  while (n > 0) {
    UInt16 k = (UInt16)(n > mtu ? mtu : n);
    IOReturn r = [ch writeSync:(void *)p length:k];
    if (r != kIOReturnSuccess) {
      logmsg("Bluetooth write failed: 0x%x", r);
      return NO;
    }
    p += k; n -= k;
  }
  return YES;
}

static void forward_rx(ZCLink *link, int cfd, size_t *back)
{
  if (link.rx.length == 0) return;
  zc_write_all(cfd, link.rx.bytes, link.rx.length, 1000);
  *back += link.rx.length;
  [link.rx setLength:0];
}

static void handle_client(int cfd, BOOL check_status)
{
  long long t0 = zc_now_ms();
  IOBluetoothDevice *dev = find_device();
  if (!dev) {
    logmsg("no paired printer matches '%s' - pair the CC4 in System Settings > Bluetooth", g_spec.UTF8String);
    abort_client(cfd);
    return;
  }

  /* A previous session may still be tearing down; reusing it gets the channel closed. */
  for (long long until = zc_now_ms() + 3000; dev.isConnected && zc_now_ms() < until;) spin(0.05);

  ZCLink *link = [ZCLink new];
  IOBluetoothRFCOMMChannel *ch = nil;
  BluetoothRFCOMMChannelID cid = find_channel(dev);
  IOReturn r = kIOReturnError;
  for (int attempt = 0; attempt < 2; attempt++) {
    r = [dev openRFCOMMChannelSync:&ch withChannelID:cid delegate:link];
    if (r == kIOReturnSuccess && ch) break;
    ch = nil;
    spin(1.0);
  }
  if (r != kIOReturnSuccess || !ch) {
    logmsg("cannot connect to %s (channel %d): 0x%x - printer off, out of range, or busy with another device?",
           dev.name.UTF8String, cid, r);
    [dev closeConnection];
    abort_client(cfd);
    return;
  }
  if (verbose) logmsg("connected to %s, channel %d, MTU %u, %.1f s", dev.name.UTF8String, cid, [ch getMTU],
                      (zc_now_ms() - t0) / 1000.0);

  if (check_status) {
    static const unsigned char q[4] = { 0x1D, 0x99, 0x00, 0x00 };
    bt_write(ch, q, sizeof(q));
    long long until = zc_now_ms() + 1500;
    while (zc_now_ms() < until && link.rx.length < 4) spin(0.05);
    const unsigned char *b = link.rx.bytes;
    for (NSUInteger i = 0; i + 3 < link.rx.length; i++) {
      if (b[i] == 0x1D && b[i + 1] == 0x99 && b[i + 3] == 0xFF) {
        unsigned s = b[i + 2];
        if (s & 0x03) {
          logmsg("printer reports %s (status 0x%02x); job refused, CUPS will retry",
                 (s & 1) ? "paper out" : "cover open", s);
          [ch closeChannel];
          [dev closeConnection];
          abort_client(cfd);
          return;
        }
        if (verbose) logmsg("status OK");
        break;
      }
    }
    [link.rx setLength:0];
  }

  unsigned char buf[16384];
  size_t total = 0, back = 0;
  BOOL failed = NO;
  for (;;) {
    struct pollfd p = { cfd, POLLIN, 0 };
    int pr = poll(&p, 1, 20);
    if (pr > 0) {
      ssize_t n = read(cfd, buf, sizeof(buf));
      if (n > 0) {
        if (!bt_write(ch, buf, (size_t)n)) { failed = YES; break; }
        total += (size_t)n;
      } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
        break;
      }
    }
    spin(0.0);
    forward_rx(link, cfd, &back);
    if (link.closed) { logmsg("printer closed the Bluetooth link after %zu bytes", total); failed = YES; break; }
  }

  if (!failed) {
    long long until = zc_now_ms() + 1000; /* vendor SDK waits 1 s before disconnecting */
    while (zc_now_ms() < until) { spin(0.05); forward_rx(link, cfd, &back); }
  }
  [ch closeChannel];
  [dev closeConnection];
  for (long long until = zc_now_ms() + 3000; dev.isConnected && zc_now_ms() < until;) spin(0.05);
  if (failed) { abort_client(cfd); return; }
  close(cfd);
  logmsg("job done: %zu bytes to printer, %zu bytes back, %.1f s", total, back, (zc_now_ms() - t0) / 1000.0);
}

static int listen_on(int port)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in sa = { 0 };
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
    logmsg("cannot listen on 127.0.0.1:%d: %s", port, strerror(errno));
    exit(1);
  }
  return fd;
}

int main(int argc, char *argv[])
{
  @autoreleasepool {
    int port = 9101, cport = 9102, check_status = 0, c;
    while ((c = getopt(argc, argv, "a:c:p:P:sv")) != -1) {
      switch (c) {
        case 'a': g_spec = [NSString stringWithUTF8String:optarg]; break;
        case 'c': g_channel = atoi(optarg); break;
        case 'p': port = atoi(optarg); break;
        case 'P': cport = atoi(optarg); break;
        case 's': check_status = 1; break;
        case 'v': verbose = 1; break;
        default:
          fprintf(stderr, "Usage: %s [-a auto|name|address] [-c channel] [-p port] [-P control-port] [-s] [-v]\n", argv[0]);
          return 2;
      }
    }
    signal(SIGPIPE, SIG_IGN);

    /* Touch Bluetooth at start-up so macOS asks for permission now, not mid-job. */
    IOBluetoothDevice *dev = find_device();
    if (dev) logmsg("printer: %s (%s)", dev.name.UTF8String, dev.addressString.UTF8String);
    else logmsg("no paired printer matches '%s' yet", g_spec.UTF8String);

    int lp = listen_on(port), lc = listen_on(cport);
    logmsg("listening on 127.0.0.1:%d (print%s) and :%d (control)", port, check_status ? ", status check" : "", cport);

    for (;;) {
      @autoreleasepool {
        struct pollfd p[2] = { { lp, POLLIN, 0 }, { lc, POLLIN, 0 } };
        if (poll(p, 2, 200) > 0) {
          for (int i = 0; i < 2; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            int cfd = accept(p[i].fd, NULL, NULL);
            if (cfd < 0) continue;
            int one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            handle_client(cfd, i == 0 && check_status);
          }
        }
        spin(0.01);
      }
    }
  }
}
