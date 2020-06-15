#include <string.h>

#include <string>
#include <fcntl.h>
#include <errno.h>
#include <sudo.h>
#include <utils.h>

#include "libarch_utils.h"

LibArchOpenRead::LibArchOpenRead(const char *name, const char *cmd)
	:
	_arc(archive_read_new()),
	_fd(sdc_open(name, O_RDONLY))
{
	if (!_arc || _fd == -1) {
		EnsureClosed();
		throw std::runtime_error(StrPrintf("open archive error %d", errno));
	}

	LibArchCall(archive_read_support_filter_all, _arc);
	LibArchCall(archive_read_support_format_all, _arc);
	LibArchCall(archive_read_support_format_raw, _arc);

	LibArchCall(archive_read_set_callback_data, _arc, (void *)this);
	LibArchCall(archive_read_set_read_callback, _arc, sReadCallback);
	LibArchCall(archive_read_set_seek_callback, _arc, sSeekCallback);
	LibArchCall(archive_read_set_skip_callback, _arc, sSkipCallback);
	LibArchCall(archive_read_set_close_callback, _arc, sCloseCallback);

	int r = LibArchCall(archive_read_open1, _arc);
	if (r != ARCHIVE_OK) {
		EnsureClosed();
		throw std::runtime_error(StrPrintf("open archive error %d", r));
	}

	_ae = NextHeader();
	_fmt = archive_format(_arc);
	if (_fmt == ARCHIVE_FORMAT_RAW && _ae != nullptr) {
		const char *name_part = strrchr(name, '/');
		std::string arc_raw_name(name_part ? name_part + 1 : name);
		size_t p = arc_raw_name.rfind('.');
		if (p != std::string::npos) {
			arc_raw_name.resize(p);
		} else {
			arc_raw_name+= ".data";
		}
		archive_entry_set_pathname(_ae, arc_raw_name.c_str() );
	}
}

LibArchOpenRead::~LibArchOpenRead()
{
	EnsureClosed();
}

struct archive_entry *LibArchOpenRead::NextHeader()
{
	struct archive_entry *entry = nullptr;
	if (_ae != nullptr) {
		entry = _ae;
		_ae = nullptr;

	} else {
		int r = LibArchCall(archive_read_next_header, _arc, &entry);

		if (r == ARCHIVE_EOF) {
			return nullptr;
		}

		if (r != ARCHIVE_OK) {
			throw std::runtime_error(StrPrintf("archive_read_next_header - error %d\n", r));
		}
	}

	return entry;
}

void LibArchOpenRead::SkipData()
{
	LibArchCall(archive_read_data_skip, _arc);
}

void LibArchOpenRead::EnsureClosed()
{
	if (_arc != nullptr) {
		archive_read_free(_arc);
		_arc = nullptr;
	}
	EnsureClosedFD();
}

void LibArchOpenRead::EnsureClosedFD()
{
	if (_fd != -1) {
		sdc_close(_fd);
		_fd = -1;
		_pos = 0;
	}
}

__LA_SSIZE_T LibArchOpenRead::sReadCallback(struct archive *, void *it, const void **_buffer)
{
	*_buffer = ((LibArchOpenRead *)it)->_buf;
	ssize_t r = sdc_read(((LibArchOpenRead *)it)->_fd, ((LibArchOpenRead *)it)->_buf, sizeof(((LibArchOpenRead *)it)->_buf));
	if (r > 0) {
		((LibArchOpenRead *)it)->_pos+= r;
	}
	return (r < 0) ? ARCHIVE_FATAL : r;
}

__LA_INT64_T LibArchOpenRead::sSkipCallback(struct archive *a, void *it, __LA_INT64_T request)
{
	__LA_INT64_T prev_pos = ((LibArchOpenRead *)it)->_pos;
	sSeekCallback(a, it, request, SEEK_CUR);
	return ((LibArchOpenRead *)it)->_pos - prev_pos;
}

__LA_INT64_T LibArchOpenRead::sSeekCallback(struct archive *, void *it, __LA_INT64_T offset, int whence)
{
	off_t r = sdc_lseek(((LibArchOpenRead *)it)->_fd, offset, whence);
	if (r < 0) {
		r = sdc_lseek(((LibArchOpenRead *)it)->_fd, 0, SEEK_CUR);
		if (r >= 0) {
			((LibArchOpenRead *)it)->_pos = r;
		}
		return ARCHIVE_FATAL;
	}
	((LibArchOpenRead *)it)->_pos = r;
	return r;
}

int LibArchOpenRead::sCloseCallback(struct archive *, void *it)
{
	((LibArchOpenRead *)it)->EnsureClosedFD();
	return 0;
}

///////////////////////////////////////////////

static const char *NameExt(const char *name)
{
	const char *slash = strrchr(name, '/');

	const char *ext = strchr(slash ? slash + 1 : name, '.');

	return ext ? ext : (slash ? slash + 1 : name);
}

LibArchOpenWrite::LibArchOpenWrite(const char *name, const char *cmd)
{
	const char *ne = NameExt(name);

	int format = ARCHIVE_FORMAT_TAR, filter = ARCHIVE_FILTER_GZIP; // defaults

	if (strstr(cmd, ":cpio")) { format = ARCHIVE_FORMAT_CPIO;
	} else if (strstr(cmd, ":zip")) { format = ARCHIVE_FORMAT_ZIP;
	} else if (strstr(cmd, ":cab")) { format = ARCHIVE_FORMAT_CAB;
	} else if (strstr(cmd, ":rar")) { format = ARCHIVE_FORMAT_RAR;
	} else if (strstr(ne, ".cpio")) { format = ARCHIVE_FORMAT_CPIO;
	} else if (strstr(ne, ".zip")) { format = ARCHIVE_FORMAT_ZIP;
	} else if (strstr(ne, ".cab")) { format = ARCHIVE_FORMAT_CAB;
	} else if (strstr(ne, ".rar")) { format = ARCHIVE_FORMAT_RAR;
	}

	if (strstr(cmd, ":plain")) { filter = 0;
	} else if (strstr(cmd, ":bz")) { filter = ARCHIVE_FILTER_BZIP2;
	} else if (strstr(cmd, ":lzip")) { filter = ARCHIVE_FILTER_LZIP;
	} else if (strstr(cmd, ":xz")) { filter = ARCHIVE_FILTER_XZ;
	} else if (strstr(cmd, ":uu")) { filter = ARCHIVE_FILTER_UU;
	} else if (strstr(cmd, ":rpm")) { filter = ARCHIVE_FILTER_RPM;
	} else if (strstr(cmd, ":lz")) { filter = ARCHIVE_FILTER_LZMA;
	} else if (strstr(ne, ".bz")) { filter = ARCHIVE_FILTER_BZIP2;
	} else if (strstr(ne, ".lzip")) { filter = ARCHIVE_FILTER_LZIP;
	} else if (strstr(ne, ".uue")) { filter = ARCHIVE_FILTER_UU;
	} else if (strstr(ne, ".rpm")) { filter = ARCHIVE_FILTER_RPM;
	} else if (strstr(ne, ".lz")) { filter = ARCHIVE_FILTER_LZMA;
	}

	_arc = archive_write_new();
	if (!_arc) {
		throw std::runtime_error("failed to init archiver");
	}
	archive_write_set_format(_arc, format);
	if (filter) {
		archive_write_add_filter(_arc, filter);
	}

	int r = LibArchCall(archive_write_open_filename, _arc, name);
	if (r != ARCHIVE_OK) {
		archive_write_free(_arc);
		throw std::runtime_error(StrPrintf("open archive error %d", r));
	}
}

LibArchOpenWrite::LibArchOpenWrite(const char *name, struct archive *arc_template)
{
	_arc = archive_write_new();
	if (!_arc) {
		throw std::runtime_error("failed to init archiver");
	}

	archive_write_set_format(_arc, archive_format(arc_template));
	for (int i = 0, ii = archive_filter_count(arc_template); i < ii; ++i) {
		int fc = archive_filter_code(arc_template, i);
		if (fc != 0) {
			archive_write_add_filter(_arc, fc);
		}
	}

	int r = LibArchCall(archive_write_open_filename, _arc, name);
	if (r != ARCHIVE_OK) {
		archive_write_free(_arc);
		throw std::runtime_error(StrPrintf("open archive error %d", r));
	}
}

LibArchOpenWrite::~LibArchOpenWrite()
{
	archive_write_close(_arc);
	archive_write_free(_arc);
}


bool LibArchOpenWrite::WriteData(const void *data, size_t len)
{
	while (len) {
		ssize_t r = archive_write_data(_arc, data, len);
		if (r <= 0) {
			return false;
		}
		len-= (size_t)r;
	}

	return true;
}
