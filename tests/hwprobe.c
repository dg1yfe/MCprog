/* The one check that needs a serial PORT and cannot be done on a pty.
 *
 *     make hwprobe PORT=/dev/cu.usbserial-XXXX
 *
 * P-12/P-27's arming sequence is 500 ms with the lines down and 1300 ms with RTS up, run straight
 * through with no deadline loop to re-correct it.  nanosleep returns EINTR with the unslept
 * remainder in `rem', and discarding it shortens the #NMI pulse the radio sees -- silently, because
 * nothing downstream can tell a short pulse from a long one.  Measured on an FTDI FT232 with a
 * signal every 70 ms: 1809 ms correct, and 140 ms with the EINTR handling removed.  A 13x shortened
 * pulse on the one signal that puts the radio into programming mode.
 *
 * A pty cannot check it: pulse_rts opens with ioctl(TIOCMSET), which fails there and returns before
 * either delay is reached, so the test would measure 0 ms and pass or fail for unrelated reasons.
 *
 * There is deliberately NOTHING here about P-31d's wire-time floor.  Sleeping out a computed
 * interval is a property of nanosleep and arithmetic, not of the adapter, so a "does drain() wait
 * 1125 ms" assertion passes on any device and measures nothing -- it is covered where it belongs,
 * on a pty, in tests/test_serial.c.  The one thing real hardware DID settle about P-31d -- that no
 * portable way exists to ask whether the transmit register is empty -- was a one-off investigation,
 * and its numbers are recorded in spec.md P-31d rather than re-measured on every run.
 *
 * Nothing is transmitted that a radio would act on and nothing is read back, so this is safe with
 * the far end open or with a radio attached.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "mc/protocol.h"
#include "mc/serial.h"

static int pass, fail;
static volatile sig_atomic_t ticks;
static void tick(int sig) { (void)sig; ticks++; }

static void ok(int cond, const char *req, const char *what)
{
	if (cond) { pass++; printf("ok    [%s] %s\n", req, what); }
	else      { fail++; printf("FAIL  [%s] %s\n", req, what); }
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : NULL;
	mc_serial_opts o;
	char err[160];
	mc_transport *t;
	unsigned t0, dt;
	struct itimerval iv;
	struct sigaction sa, old;

	if (!dev) { fprintf(stderr, "usage: hwprobe /dev/...\n"); return 2; }
	mc_serial_defaults(&o);
	o.line_setup = 0; /* no pulse on open; this drives it explicitly and times it */
	t = mc_serial_open(dev, &o, err, sizeof err);
	if (!t) { fprintf(stderr, "hwprobe: %s\n", err); return 2; }
	printf("port %s\n\n", dev);

	if (mc_serial_rearm(t) != 0) {
		printf("this port has no control lines, so the arming delays cannot be timed here.\n");
		mc_serial_close(t);
		return 2;
	}
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = tick;      /* a real handler, so the sleeps are interrupted */
	sigaction(SIGALRM, &sa, &old);
	ticks = 0;
	iv.it_value.tv_sec = 0;    iv.it_value.tv_usec = 70000;
	iv.it_interval.tv_sec = 0; iv.it_interval.tv_usec = 70000;
	setitimer(ITIMER_REAL, &iv, NULL);

	t0 = t->now_ms(t);
	mc_serial_rearm(t);
	dt = t->now_ms(t) - t0;

	memset(&iv, 0, sizeof iv);
	setitimer(ITIMER_REAL, &iv, NULL);
	sigaction(SIGALRM, &old, NULL);

	printf("      arming took %u ms with %d signals delivered; 500 + 1300 = 1800 required\n",
	       dt, (int)ticks);
	ok(ticks > 1, "P-27", "the sequence really was interrupted, repeatedly");
	ok(dt + 20 >= 1800, "P-27",
	   "500 ms down + 1300 ms up survives signals -- the #NMI pulse is not shortened");

	mc_serial_close(t);
	printf("\n%d passed, %d FAILED\n", pass, fail);
	return fail ? 1 : 0;
}
