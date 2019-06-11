#include <utils.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>

#include <ScopeHelpers.h>
#include <TTYRawMode.h>

#include "../Globals.h"
#include "OpExecute.h"

static int g_fd_ctl = -1;

static bool FD2FD(int dst, int src)
{
	char buf[32768];
	ssize_t r = read(src, buf, sizeof(buf));
	if (r < 0) {
		if (errno == EAGAIN)
			return true;

		throw std::runtime_error("FD2FD: read failed");
	}

	if (r == 0) {
		return false;
	}

	if (WriteAll(dst, buf, (size_t)r) != (size_t)r) {
		throw std::runtime_error("FD2FD: write failed");
	}

	return true;
}

static void FDCtl_VTSize()
{
	char c = 0;
	if (WriteAll(g_fd_ctl, &c, 1) != 1) {
		throw std::runtime_error("FDCtl_VTSize: header failed");
	}
	unsigned int cols = 80, rows = 25;
	struct winsize w = {};
	if (ioctl(1, TIOCGWINSZ, &w) == 0) {
		cols = w.ws_col;
		rows = w.ws_row;
	}
	if (WriteAll(g_fd_ctl, &cols, sizeof(cols)) != sizeof(cols)
	 || WriteAll(g_fd_ctl, &rows, sizeof(rows)) != sizeof(rows)) {
		throw std::runtime_error("FDCtl_VTSize: data failed");
	}
}

static void sigwinch_handler(int)
{
	FDCtl_VTSize();
}

SHAREDSYMBOL int OpExecute_Shell(int argc, char *argv[])
{
//	fprintf(stderr, "OpExecute_Shell: ENTER\n");

	try {
		if (argc < 1) {
			throw std::runtime_error("Missing argument");
		}

		std::string fifo = argv[0];

		int fd_stdin = 0, fd_stdout = 1, fd_stderr = 2;

		TTYRawMode tty_raw_mode(fd_stdout);

		FDScope fd_err(open((fifo + ".err").c_str(), O_RDONLY));
		FDScope fd_out(open((fifo + ".out").c_str(), O_RDONLY));
		FDScope fd_in(open((fifo + ".in").c_str(), O_WRONLY));
		FDScope fd_ctl(open((fifo + ".ctl").c_str(), O_WRONLY));
		
		if (!fd_ctl.Valid() || !fd_in.Valid() || !fd_out.Valid() || !fd_err.Valid()) {
			throw std::runtime_error("Can't open FIFO");
		}

		g_fd_ctl = fd_ctl;
		FDCtl_VTSize();
		signal(SIGWINCH,  sigwinch_handler);

		fd_set fdr, fde;
		for (;;) {
			FD_ZERO(&fdr);
			FD_ZERO(&fde);
			FD_SET(fd_out, &fdr);
			FD_SET(fd_err, &fdr);
			FD_SET(fd_stdin, &fdr);
			FD_SET(fd_out, &fde);
			FD_SET(fd_err, &fde);
			FD_SET(fd_stdin, &fde);
			int r = select(std::max(fd_stdin, std::max((int)fd_out, (int)fd_err)) + 1, &fdr, nullptr, &fde, NULL);
			if ( r < 0) {
				if (errno == EAGAIN)
					continue;

				throw std::runtime_error("select failed");
			}

			if (FD_ISSET(fd_stdin, &fdr) || FD_ISSET(fd_stdin, &fde)) {
				if (!FD2FD(fd_in, fd_stdin))
					throw std::runtime_error("read input failed");
			}

			int out_fails = 0;
			if (FD_ISSET(fd_out, &fdr) || FD_ISSET(fd_out, &fde)) {
				if (!FD2FD(fd_stdout, fd_out))
					out_fails|= 1;
			}

			if (FD_ISSET(fd_err, &fdr) || FD_ISSET(fd_err, &fde)) {
				if (!FD2FD(fd_stderr, fd_err))
					out_fails|= 2;
			}

			if (out_fails == 3) {
				break;

			} else if (out_fails) {
				usleep(10000);
			}
		}



	} catch (std::exception &ex) {
		fprintf(stderr, "OpExecute_Shell: %s\n", ex.what());
		g_fd_ctl = -1;
		return -1;
	}

//	fprintf(stderr, "OpExecute_Shell: LEAVE\n");
	g_fd_ctl = -1;
	return 0;
}


OpExecute::OpExecute(std::shared_ptr<IHost> &host, const std::string &dir, const std::string &command)
	:
	_host(host),
	_dir(dir),
	_command(command)
{
	char sz[64] = {};
	snprintf(sz, sizeof(sz) - 1, "NetRocks/fifo/%lx", (unsigned long)getpid());

	_fifo = InMyTemp(sz);

	CleanupFIFO();

	if (mkfifo((_fifo + ".ctl").c_str(), 0700) == -1 ) {
		throw std::runtime_error("mkfifo ctl");
	}
	if (mkfifo((_fifo + ".in").c_str(), 0700) == -1 ) {
		CleanupFIFO();
		throw std::runtime_error("mkfifo in");
	}
	if (mkfifo((_fifo + ".out").c_str(), 0700) == -1) {
		CleanupFIFO();
		throw std::runtime_error("mkfifo out");
	}
	if (mkfifo((_fifo + ".err").c_str(), 0700) == -1) {
		CleanupFIFO();
		throw std::runtime_error("mkfifo err");
	}
}

OpExecute::~OpExecute()
{
	CleanupFIFO();
}

void OpExecute::CleanupFIFO()
{
	{
		FDScope fd_ctl(open((_fifo + ".ctl").c_str(), O_WRONLY | O_NONBLOCK));
		FDScope fd_in(open((_fifo + ".in").c_str(), O_WRONLY | O_NONBLOCK));
		FDScope fd_out(open((_fifo + ".out").c_str(), O_RDONLY | O_NONBLOCK));
		FDScope fd_err(open((_fifo + ".err").c_str(), O_RDONLY | O_NONBLOCK));
	}

	unlink((_fifo + ".ctl").c_str());
	unlink((_fifo + ".in").c_str());
	unlink((_fifo + ".out").c_str());
	unlink((_fifo + ".err").c_str());
}


bool OpExecute::Do()
{
	try {
		_host->ExecuteCommand(_dir, _command, _fifo);
		G.info.FSF->ExecuteLibrary(G.plugin_path.c_str(), L"OpExecute_Shell", StrMB2Wide(_fifo).c_str(), 0);

	} catch (std::exception &ex)
	{
		fprintf(stderr, "OpExecute::Do: %s\n", ex.what());
	}
	return true;
}

