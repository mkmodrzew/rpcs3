#include "File.h"
#include "mutex.h"
#include "StrFmt.h"
#include "Crypto/sha1.h"

#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <map>

#include "util/asm.hpp"

using namespace std::literals::string_literals;

#ifdef _WIN32

#include <cwchar>
#include <Windows.h>

static std::unique_ptr<wchar_t[]> to_wchar(const std::string& source)
{
	// String size + null terminator
	const usz buf_size = source.size() + 1;

	// Safe size
	const int size = narrow<int>(buf_size);

	// Buffer for max possible output length
	std::unique_ptr<wchar_t[]> buffer(new wchar_t[buf_size + 8 + 32768]);

	// Prepend wide path prefix (4 characters)
	std::memcpy(buffer.get() + 32768, L"\\\\\?\\", 4 * sizeof(wchar_t));

	// Test whether additional UNC prefix is required
	const bool unc = source.size() > 2 && (source[0] == '\\' || source[0] == '/') && source[1] == source[0];

	if (unc)
	{
		// Use \\?\UNC\ prefix
		std::memcpy(buffer.get() + 32768 + 4, L"UNC\\", 4 * sizeof(wchar_t));
	}

	ensure(MultiByteToWideChar(CP_UTF8, 0, source.c_str(), size, buffer.get() + 32768 + (unc ? 8 : 4), size)); // "to_wchar"

	// Canonicalize wide path (replace '/', ".", "..", \\ repetitions, etc)
	ensure(GetFullPathNameW(buffer.get() + 32768, 32768, buffer.get(), nullptr) - 1 < 32768 - 1); // "to_wchar"

	return buffer;
}

static void to_utf8(std::string& out, const wchar_t* source)
{
	// String size
	const usz length = std::wcslen(source);

	// Safe buffer size for max possible output length (including null terminator)
	const int buf_size = narrow<int>(length * 3 + 1);

	// Resize buffer
	out.resize(buf_size - 1);

	const int result = WideCharToMultiByte(CP_UTF8, 0, source, static_cast<int>(length) + 1, &out.front(), buf_size, nullptr, nullptr);

	// Fix the size
	out.resize(ensure(result) - 1);
}

static time_t to_time(const ULARGE_INTEGER& ft)
{
	return ft.QuadPart / 10000000ULL - 11644473600ULL;
}

static time_t to_time(const LARGE_INTEGER& ft)
{
	ULARGE_INTEGER v;
	v.LowPart = ft.LowPart;
	v.HighPart = ft.HighPart;

	return to_time(v);
}

static time_t to_time(const FILETIME& ft)
{
	ULARGE_INTEGER v;
	v.LowPart = ft.dwLowDateTime;
	v.HighPart = ft.dwHighDateTime;

	return to_time(v);
}

static FILETIME from_time(s64 _time)
{
	FILETIME result;

	if (_time <= -11644473600ll)
	{
		result.dwLowDateTime = 0;
		result.dwHighDateTime = 0;
	}
	else if (_time > INT64_MAX / 10000000ll - 11644473600ll)
	{
		result.dwLowDateTime = 0xffffffff;
		result.dwHighDateTime = 0x7fffffff;
	}
	else
	{
		const ullong wtime = (_time + 11644473600ull) * 10000000ull;
		result.dwLowDateTime = static_cast<DWORD>(wtime);
		result.dwHighDateTime = static_cast<DWORD>(wtime >> 32);
	}

	return result;
}

static fs::error to_error(DWORD e)
{
	switch (e)
	{
	case ERROR_FILE_NOT_FOUND: return fs::error::noent;
	case ERROR_PATH_NOT_FOUND: return fs::error::noent;
	case ERROR_ACCESS_DENIED: return fs::error::acces;
	case ERROR_ALREADY_EXISTS: return fs::error::exist;
	case ERROR_FILE_EXISTS: return fs::error::exist;
	case ERROR_NEGATIVE_SEEK: return fs::error::inval;
	case ERROR_DIRECTORY: return fs::error::inval;
	case ERROR_INVALID_NAME: return fs::error::inval;
	case ERROR_SHARING_VIOLATION: return fs::error::acces;
	case ERROR_DIR_NOT_EMPTY: return fs::error::notempty;
	case ERROR_NOT_READY: return fs::error::noent;
	case ERROR_FILENAME_EXCED_RANGE: return fs::error::toolong;
	case ERROR_DISK_FULL: return fs::error::nospace;
	default: return fs::error::unknown;
	}
}

#else

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>

#if defined(__APPLE__)
#include <copyfile.h>
#include <mach-o/dyld.h>
#elif defined(__linux__) || defined(__sun)
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#else
#include <fstream>
#endif

static fs::error to_error(int e)
{
	switch (e)
	{
	case ENOENT: return fs::error::noent;
	case EEXIST: return fs::error::exist;
	case EINVAL: return fs::error::inval;
	case EACCES: return fs::error::acces;
	case ENOTEMPTY: return fs::error::notempty;
	case EROFS: return fs::error::readonly;
	case EISDIR: return fs::error::isdir;
	case ENOSPC: return fs::error::nospace;
	default: return fs::error::unknown;
	}
}

#endif

static std::string path_append(std::string_view path, std::string_view more)
{
	std::string result;

	if (const usz src_slash_pos = path.find_last_not_of('/'); src_slash_pos != path.npos)
	{
		path.remove_suffix(path.length() - src_slash_pos - 1);
		result = path;
	}

	result.push_back('/');

	if (const usz dst_slash_pos = more.find_first_not_of('/'); dst_slash_pos != more.npos)
	{
		more.remove_prefix(dst_slash_pos);
		result.append(more);
	}

	return result;
}

namespace fs
{
	thread_local error g_tls_error = error::ok;

	class device_manager final
	{
		mutable shared_mutex m_mutex{};

		std::unordered_map<std::string, std::shared_ptr<device_base>> m_map{};

	public:
		std::shared_ptr<device_base> get_device(const std::string& path);
		std::shared_ptr<device_base> set_device(const std::string& name, const std::shared_ptr<device_base>&);
	};

	static device_manager& get_device_manager()
	{
		// Use magic static
		static device_manager instance;
		return instance;
	}

	file_base::~file_base()
	{
	}

	[[noreturn]] stat_t file_base::stat()
	{
		fmt::throw_exception("fs::file::stat() not supported.");
	}

	void file_base::sync()
	{
		// Do notning
	}

	fs::native_handle fs::file_base::get_handle()
	{
#ifdef _WIN32
		return INVALID_HANDLE_VALUE;
#else
		return -1;
#endif
	}

	u64 file_base::write_gather(const iovec_clone* buffers, u64 buf_count)
	{
		u64 total = 0;

		for (u64 i = 0; i < buf_count; i++)
		{
			if (!buffers[i].iov_base || buffers[i].iov_len + total < total)
			{
				g_tls_error = error::inval;
				return -1;
			}

			total += buffers[i].iov_len;
		}

		const auto buf = std::make_unique<uchar[]>(total);

		u64 copied = 0;

		for (u64 i = 0; i < buf_count; i++)
		{
			std::memcpy(buf.get() + copied, buffers[i].iov_base, buffers[i].iov_len);
			copied += buffers[i].iov_len;
		}

		return this->write(buf.get(), total);
	}

	dir_base::~dir_base()
	{
	}

	device_base::~device_base()
	{
	}
}

std::shared_ptr<fs::device_base> fs::device_manager::get_device(const std::string& path)
{
	reader_lock lock(m_mutex);

	const auto found = m_map.find(path.substr(0, path.find_first_of('/', 2)));

	if (found == m_map.end())
	{
		return nullptr;
	}

	return found->second;
}

std::shared_ptr<fs::device_base> fs::device_manager::set_device(const std::string& name, const std::shared_ptr<device_base>& device)
{
	std::lock_guard lock(m_mutex);

	return m_map[name] = device;
}

std::shared_ptr<fs::device_base> fs::get_virtual_device(const std::string& path)
{
	// Every virtual device path must have "//" at the beginning
	if (path.starts_with("//"))
	{
		return get_device_manager().get_device(path);
	}

	return nullptr;
}

std::shared_ptr<fs::device_base> fs::set_virtual_device(const std::string& name, const std::shared_ptr<device_base>& device)
{
	ensure(name.starts_with("//") && name[2] != '/');

	return get_device_manager().set_device(name, device);
}

std::string fs::get_parent_dir(const std::string& path)
{
	std::string_view result = path;

	// Number of path components to remove
	usz to_remove = 1;

	while (to_remove--)
	{
		// Trim contiguous delimiters at the end
		if (usz sz = result.find_last_not_of(delim) + 1)
		{
			result = result.substr(0, sz);
		}
		else
		{
			return "/";
		}

		const auto elem = result.substr(result.find_last_of(delim) + 1);

		if (elem.empty() || elem.size() == result.size())
		{
			break;
		}

		if (elem == ".")
		{
			to_remove += 1;
		}

		if (elem == "..")
		{
			to_remove += 2;
		}

		result.remove_suffix(elem.size());
	}

	if (usz sz = result.find_last_not_of(delim) + 1)
	{
		result = result.substr(0, sz);
	}
	else
	{
		return "/";
	}

	return std::string{result};
}

bool fs::stat(const std::string& path, stat_t& info)
{
	if (auto device = get_virtual_device(path))
	{
		return device->stat(path, info);
	}

#ifdef _WIN32
	std::string_view epath = path;

	// '/' and '\\' Not allowed by FindFirstFileExW at the end of path but we should allow it
	if (auto not_del = epath.find_last_not_of(delim); not_del != umax && not_del != epath.size() - 1)
	{
		epath.remove_suffix(epath.size() - 1 - not_del);
	}

	// Handle drives specially
	if (epath.find_first_of(delim) == umax && epath.ends_with(':'))
	{
		WIN32_FILE_ATTRIBUTE_DATA attrs;

		// Must end with a delimiter
		if (!GetFileAttributesExW(to_wchar(std::string(epath) + '/').get(), GetFileExInfoStandard, &attrs))
		{
			g_tls_error = to_error(GetLastError());
			return false;
		}

		info.is_directory = true; // Handle drives as directories
		info.is_writable = (attrs.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0;
		info.size = attrs.nFileSizeLow | (u64{attrs.nFileSizeHigh} << 32);
		info.atime = to_time(attrs.ftLastAccessTime);
		info.mtime = to_time(attrs.ftLastWriteTime);
		info.ctime = info.mtime;

		if (info.atime < info.mtime)
			info.atime = info.mtime;

		return true;
	}

	WIN32_FIND_DATA attrs;

	// Allowed by FindFirstFileExW but we should not allow it
	if (epath.ends_with("*"))
	{
		g_tls_error = fs::error::noent;
		return false;
	}

	const auto wchar_ptr = to_wchar(std::string(epath));
	const std::wstring_view wpath_view = wchar_ptr.get();

	const HANDLE handle = FindFirstFileExW(wpath_view.data(), FindExInfoStandard, &attrs, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_CASE_SENSITIVE);

	if (handle == INVALID_HANDLE_VALUE)
	{
		g_tls_error = to_error(GetLastError());
		return false;
	}

	struct close_t
	{
		HANDLE handle;
		~close_t() { FindClose(handle); }
	};

	for (close_t find_manage{handle}; attrs.cFileName != wpath_view.substr(wpath_view.find_last_of(wdelim) + 1);)
	{
		if (!FindNextFileW(handle, &attrs))
		{
			if (const DWORD err = GetLastError(); err != ERROR_NO_MORE_FILES)
			{
				g_tls_error = to_error(err);
				return false;
			}

			g_tls_error = fs::error::noent;
			return false;
		}
	}

	info.is_directory = (attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	info.is_writable = (attrs.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0;
	info.size = attrs.nFileSizeLow | (u64{attrs.nFileSizeHigh} << 32);
	info.atime = to_time(attrs.ftLastAccessTime);
	info.mtime = to_time(attrs.ftLastWriteTime);
	info.ctime = info.mtime;
#else
	struct ::stat file_info;
	if (::stat(path.c_str(), &file_info) != 0)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	info.is_directory = S_ISDIR(file_info.st_mode);
	info.is_writable = file_info.st_mode & 0200; // HACK: approximation
	info.size = file_info.st_size;
	info.atime = file_info.st_atime;
	info.mtime = file_info.st_mtime;
	info.ctime = info.mtime;
#endif

	if (info.atime < info.mtime)
		info.atime = info.mtime;

	return true;
}

bool fs::exists(const std::string& path)
{
	fs::stat_t info{};
	return fs::stat(path, info);
}

bool fs::is_file(const std::string& path)
{
	fs::stat_t info{};
	if (!fs::stat(path, info))
	{
		return false;
	}

	if (info.is_directory)
	{
		g_tls_error = error::exist;
		return false;
	}

	return true;
}

bool fs::is_dir(const std::string& path)
{
	fs::stat_t info{};
	if (!fs::stat(path, info))
	{
		return false;
	}

	if (!info.is_directory)
	{
		g_tls_error = error::exist;
		return false;
	}

	return true;
}

bool fs::statfs(const std::string& path, fs::device_stat& info)
{
	if (auto device = get_virtual_device(path))
	{
		return device->statfs(path, info);
	}

#ifdef _WIN32
	ULARGE_INTEGER avail_free;
	ULARGE_INTEGER total_size;
	ULARGE_INTEGER total_free;

	// Convert path and return it back to the "short" format
	const bool unc = path.size() > 2 && (path[0] == '\\' || path[0] == '/') && path[1] == path[0];

	std::wstring str = to_wchar(path).get() + (unc ? 6 : 4);

	if (unc)
	{
		str[0] = '\\';
		str[1] = '\\';
	}

	// Keep cutting path from right until it's short enough
	while (str.size() > 256)
	{
		if (usz x = str.find_last_of('\\') + 1)
			str.resize(x - 1);
		else
			break;
	}

	if (!GetDiskFreeSpaceExW(str.c_str(), &avail_free, &total_size, &total_free))
	{
		g_tls_error = to_error(GetLastError());
		return false;
	}

	info.block_size = 4096; // TODO
	info.total_size = total_size.QuadPart;
	info.total_free = total_free.QuadPart;
	info.avail_free = avail_free.QuadPart;
#else
	struct ::statvfs buf;
	if (::statvfs(path.c_str(), &buf) != 0)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	info.block_size = buf.f_frsize;
	info.total_size = info.block_size * buf.f_blocks;
	info.total_free = info.block_size * buf.f_bfree;
	info.avail_free = info.block_size * buf.f_bavail;
#endif

	return true;
}

bool fs::create_dir(const std::string& path)
{
	if (auto device = get_virtual_device(path))
	{
		return device->create_dir(path);
	}

#ifdef _WIN32
	if (!CreateDirectoryW(to_wchar(path).get(), nullptr))
	{
		int res = GetLastError();

		if (res == ERROR_ACCESS_DENIED && is_dir(path))
		{
			// May happen on drives
			res = ERROR_ALREADY_EXISTS;
		}

		g_tls_error = to_error(res);
		return false;
	}

	return true;
#else
	if (::mkdir(path.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	return true;
#endif
}

bool fs::create_path(const std::string& path)
{
	const std::string parent = get_parent_dir(path);

#ifdef _WIN32
	// Workaround: don't call is_dir with naked drive letter
	if (!parent.empty() && parent.back() != ':' && !is_dir(parent) && !create_path(parent))
#else
	if (!parent.empty() && !is_dir(parent) && !create_path(parent))
#endif
	{
		return false;
	}

	if (!create_dir(path) && g_tls_error != error::exist)
	{
		return false;
	}

	return true;
}

bool fs::remove_dir(const std::string& path)
{
	if (path.empty())
	{
		// Don't allow removing empty path (TODO)
		g_tls_error = fs::error::noent;
		return false;
	}

	if (auto device = get_virtual_device(path))
	{
		return device->remove_dir(path);
	}

#ifdef _WIN32
	if (!RemoveDirectoryW(to_wchar(path).get()))
	{
		g_tls_error = to_error(GetLastError());
		return false;
	}

	return true;
#else
	if (::rmdir(path.c_str()) != 0)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	return true;
#endif
}

bool fs::rename(const std::string& from, const std::string& to, bool overwrite)
{
	if (from.empty() || to.empty())
	{
		// Don't allow opening empty path (TODO)
		g_tls_error = fs::error::noent;
		return false;
	}

	const auto device = get_virtual_device(from);

	if (device != get_virtual_device(to))
	{
		fmt::throw_exception("fs::rename() between different devices not implemented.\nFrom: %s\nTo: %s", from, to);
	}

	if (device)
	{
		return device->rename(from, to);
	}

#ifdef _WIN32
	const auto ws1 = to_wchar(from);
	const auto ws2 = to_wchar(to);

	if (!MoveFileExW(ws1.get(), ws2.get(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0))
	{
		DWORD error1 = GetLastError();

		if (overwrite && error1 == ERROR_ACCESS_DENIED && is_dir(from) && is_dir(to))
		{
			if (RemoveDirectoryW(ws2.get()))
			{
				if (MoveFileW(ws1.get(), ws2.get()))
				{
					return true;
				}

				error1 = GetLastError();
				CreateDirectoryW(ws2.get(), nullptr); // TODO
			}
			else
			{
				error1 = GetLastError();
			}
		}

		g_tls_error = to_error(error1);
		return false;
	}

	return true;
#else

#ifdef __linux__
	if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), overwrite ? 0 : 1 /* RENAME_NOREPLACE */) == 0)
	{
		return true;
	}

	// If the filesystem doesn't support RENAME_NOREPLACE, it returns EINVAL. Retry with fallback method in that case.
	if (errno != EINVAL || overwrite)
	{
		g_tls_error = to_error(errno);
		return false;
	}
#endif

	if (!overwrite && exists(to))
	{
		g_tls_error = fs::error::exist;
		return false;
	}

	if (::rename(from.c_str(), to.c_str()) != 0)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	return true;
#endif
}

bool fs::copy_file(const std::string& from, const std::string& to, bool overwrite)
{
	const auto device = get_virtual_device(from);

	if (device != get_virtual_device(to) || device) // TODO
	{
		fmt::throw_exception("fs::copy_file() for virtual devices not implemented.\nFrom: %s\nTo: %s", from, to);
	}

#ifdef _WIN32
	if (!CopyFileW(to_wchar(from).get(), to_wchar(to).get(), !overwrite))
	{
		g_tls_error = to_error(GetLastError());
		return false;
	}

	return true;
#elif defined(__APPLE__) || defined(__linux__) || defined(__sun)
	/* Source: http://stackoverflow.com/questions/2180079/how-can-i-copy-a-file-on-unix-using-c */

	const int input = ::open(from.c_str(), O_RDONLY);
	if (input == -1)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	const int output = ::open(to.c_str(), O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : O_EXCL), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (output == -1)
	{
		const int err = errno;

		::close(input);
		g_tls_error = to_error(err);
		return false;
	}

	// Here we use kernel-space copying for performance reasons
#if defined(__APPLE__)
	// fcopyfile works on OS X 10.5+
	if (::fcopyfile(input, output, 0, COPYFILE_ALL))
#elif defined(__linux__) || defined(__sun)
	// sendfile will work with non-socket output (i.e. regular file) on Linux 2.6.33+
	off_t bytes_copied = 0;
	struct ::stat fileinfo = { 0 };
	if (::fstat(input, &fileinfo) == -1 || ::sendfile(output, input, &bytes_copied, fileinfo.st_size) == -1)
#else
#error "Native file copy implementation is missing"
#endif
	{
		const int err = errno;

		::close(input);
		::close(output);
		g_tls_error = to_error(err);
		return false;
	}

	::close(input);
	::close(output);
	return true;
#else // fallback
	{
		std::ifstream out{to, std::ios::binary};
		if (out.good() && !overwrite)
		{
			g_tls_error = to_error(EEXIST);
			return false;
		}
	}

	std::ifstream in{from, std::ios::binary};
	std::ofstream out{to,  std::ios::binary};

	if (!in.good() || !out.good())
	{
		g_tls_error = to_error(errno);
		return false;
	}

	std::istreambuf_iterator<char> bin(in);
	std::istreambuf_iterator<char> ein;
	std::ostreambuf_iterator<char> bout(out);
	std::copy(bin, ein, bout);

	return true;
#endif
}

bool fs::remove_file(const std::string& path)
{
	if (auto device = get_virtual_device(path))
	{
		return device->remove(path);
	}

#ifdef _WIN32
	if (!DeleteFileW(to_wchar(path).get()))
	{
		g_tls_error = to_error(GetLastError());
		return false;
	}

	return true;
#else
	if (::unlink(path.c_str()) != 0)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	return true;
#endif
}

bool fs::truncate_file(const std::string& path, u64 length)
{
	if (auto device = get_virtual_device(path))
	{
		return device->trunc(path, length);
	}

#ifdef _WIN32
	// Open the file
	const auto handle = CreateFileW(to_wchar(path).get(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		g_tls_error = to_error(GetLastError());
		return false;
	}

	FILE_END_OF_FILE_INFO _eof;
	_eof.EndOfFile.QuadPart = length;

	if (!SetFileInformationByHandle(handle, FileEndOfFileInfo, &_eof, sizeof(_eof)))
	{
		g_tls_error = to_error(GetLastError());
		CloseHandle(handle);
		return false;
	}

	CloseHandle(handle);
	return true;
#else
	if (::truncate(path.c_str(), length) != 0)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	return true;
#endif
}

bool fs::utime(const std::string& path, s64 atime, s64 mtime)
{
	if (auto device = get_virtual_device(path))
	{
		return device->utime(path, atime, mtime);
	}

#ifdef _WIN32
	// Open the file
	const auto handle = CreateFileW(to_wchar(path).get(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		g_tls_error = to_error(GetLastError());
		return false;
	}

	FILETIME _atime = from_time(atime);
	FILETIME _mtime = from_time(mtime);
	if (!SetFileTime(handle, nullptr, &_atime, &_mtime))
	{
		g_tls_error = to_error(GetLastError());
		CloseHandle(handle);
		return false;
	}

	CloseHandle(handle);
	return true;
#else
	::utimbuf buf;
	buf.actime = atime;
	buf.modtime = mtime;

	if (::utime(path.c_str(), &buf) != 0)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	return true;
#endif
}

void fs::sync()
{
#ifdef _WIN32
	fs::g_tls_error = fs::error::unknown;
#else
	::sync();
	fs::g_tls_error = fs::error::ok;
#endif
}

[[noreturn]] void fs::xnull(const src_loc& loc)
{
	fmt::throw_exception("Null object.%s", loc);
}

[[noreturn]] void fs::xfail(const src_loc& loc)
{
	fmt::throw_exception("Unexpected fs::error %s%s", g_tls_error, loc);
}

[[noreturn]] void fs::xovfl()
{
	fmt::throw_exception("Stream overflow.");
}

fs::file::file(const std::string& path, bs_t<open_mode> mode)
{
	if (path.empty())
	{
		// Don't allow opening empty path (TODO)
		g_tls_error = fs::error::noent;
		return;
	}

	if (auto device = get_virtual_device(path))
	{
		if (auto&& _file = device->open(path, mode))
		{
			m_file = std::move(_file);
			return;
		}

		return;
	}

#ifdef _WIN32
	DWORD access = 0;
	if (mode & fs::read) access |= GENERIC_READ;
	if (mode & fs::write) access |= DELETE | (mode & fs::append ? FILE_APPEND_DATA : GENERIC_WRITE);

	DWORD disp = 0;
	if (mode & fs::create)
	{
		disp =
			mode & fs::excl ? CREATE_NEW :
			mode & fs::trunc ? CREATE_ALWAYS : OPEN_ALWAYS;
	}
	else
	{
		if (mode & fs::excl)
		{
			g_tls_error = error::inval;
			return;
		}

		disp = mode & fs::trunc ? TRUNCATE_EXISTING : OPEN_EXISTING;
	}

	DWORD share = FILE_SHARE_DELETE;
	if (!(mode & fs::unread) || !(mode & fs::write))
	{
		share |= FILE_SHARE_READ;
	}

	if (!(mode & (fs::lock + fs::unread)) || !(mode & fs::write))
	{
		share |= FILE_SHARE_WRITE;
	}

	const HANDLE handle = CreateFileW(to_wchar(path).get(), access, share, nullptr, disp, FILE_ATTRIBUTE_NORMAL, NULL);

	if (handle == INVALID_HANDLE_VALUE)
	{
		g_tls_error = to_error(GetLastError());
		return;
	}

	class windows_file final : public file_base
	{
		const HANDLE m_handle;

	public:
		windows_file(HANDLE handle)
			: m_handle(handle)
		{
		}

		~windows_file() override
		{
			CloseHandle(m_handle);
		}

		stat_t stat() override
		{
			FILE_BASIC_INFO basic_info;
			ensure(GetFileInformationByHandleEx(m_handle, FileBasicInfo, &basic_info, sizeof(FILE_BASIC_INFO))); // "file::stat"

			stat_t info;
			info.is_directory = (basic_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			info.is_writable = (basic_info.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0;
			info.size = this->size();
			info.atime = to_time(basic_info.LastAccessTime);
			info.mtime = to_time(basic_info.LastWriteTime);
			info.ctime = info.mtime;

			if (info.atime < info.mtime)
				info.atime = info.mtime;

			return info;
		}

		void sync() override
		{
			ensure(FlushFileBuffers(m_handle)); // "file::sync"
		}

		bool trunc(u64 length) override
		{
			FILE_END_OF_FILE_INFO _eof;
			_eof.EndOfFile.QuadPart = length;

			if (!SetFileInformationByHandle(m_handle, FileEndOfFileInfo, &_eof, sizeof(_eof)))
			{
				g_tls_error = to_error(GetLastError());
				return false;
			}

			return true;
		}

		u64 read(void* buffer, u64 count) override
		{
			// TODO (call ReadFile multiple times if count is too big)
			const int size = narrow<int>(count);

			DWORD nread;
			ensure(ReadFile(m_handle, buffer, size, &nread, nullptr)); // "file::read"

			return nread;
		}

		u64 write(const void* buffer, u64 count) override
		{
			// TODO (call WriteFile multiple times if count is too big)
			const int size = narrow<int>(count);

			DWORD nwritten;
			ensure(WriteFile(m_handle, buffer, size, &nwritten, nullptr)); // "file::write"

			return nwritten;
		}

		u64 seek(s64 offset, seek_mode whence) override
		{
			if (whence > seek_end)
			{
				fmt::throw_exception("Invalid whence (0x%x)", whence);
			}

			LARGE_INTEGER pos;
			pos.QuadPart = offset;

			const DWORD mode =
				whence == seek_set ? FILE_BEGIN :
				whence == seek_cur ? FILE_CURRENT : FILE_END;

			if (!SetFilePointerEx(m_handle, pos, &pos, mode))
			{
				g_tls_error = to_error(GetLastError());
				return -1;
			}

			return pos.QuadPart;
		}

		u64 size() override
		{
			LARGE_INTEGER size;
			ensure(GetFileSizeEx(m_handle, &size)); // "file::size"

			return size.QuadPart;
		}

		native_handle get_handle() override
		{
			return m_handle;
		}
	};

	m_file = std::make_unique<windows_file>(handle);
#else
	int flags = O_CLOEXEC; // Ensures all files are closed on execl for auto updater

	if (mode & fs::read && mode & fs::write) flags |= O_RDWR;
	else if (mode & fs::read) flags |= O_RDONLY;
	else if (mode & fs::write) flags |= O_WRONLY;

	if (mode & fs::append) flags |= O_APPEND;
	if (mode & fs::create) flags |= O_CREAT;
	if (mode & fs::trunc && !(mode & fs::lock)) flags |= O_TRUNC;
	if (mode & fs::excl) flags |= O_EXCL;

	int perm = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	if (mode & fs::write && mode & fs::unread)
	{
		if (!(mode & (fs::excl + fs::lock)) && mode & fs::trunc)
		{
			// Alternative to truncation for "unread" flag (TODO)
			if (mode & fs::create)
			{
				::unlink(path.c_str());
			}
		}

		perm = 0;
	}

	const int fd = ::open(path.c_str(), flags, perm);

	if (fd == -1)
	{
		g_tls_error = to_error(errno);
		return;
	}

	if (mode & fs::write && mode & fs::lock && ::flock(fd, LOCK_EX | LOCK_NB) != 0)
	{
		g_tls_error = errno == EWOULDBLOCK ? fs::error::acces : to_error(errno);
		::close(fd);
		return;
	}

	if (mode & fs::trunc && mode & fs::lock && mode & fs::write)
	{
		// Postpone truncation in order to avoid using O_TRUNC on a locked file
		ensure(::ftruncate(fd, 0) == 0);
	}

	class unix_file final : public file_base
	{
		const int m_fd;

	public:
		unix_file(int fd)
			: m_fd(fd)
		{
		}

		~unix_file() override
		{
			::close(m_fd);
		}

		stat_t stat() override
		{
			struct ::stat file_info;
			ensure(::fstat(m_fd, &file_info) == 0); // "file::stat"

			stat_t info;
			info.is_directory = S_ISDIR(file_info.st_mode);
			info.is_writable = file_info.st_mode & 0200; // HACK: approximation
			info.size = file_info.st_size;
			info.atime = file_info.st_atime;
			info.mtime = file_info.st_mtime;
			info.ctime = info.mtime;

			if (info.atime < info.mtime)
				info.atime = info.mtime;

			return info;
		}

		void sync() override
		{
			ensure(::fsync(m_fd) == 0); // "file::sync"
		}

		bool trunc(u64 length) override
		{
			if (::ftruncate(m_fd, length) != 0)
			{
				g_tls_error = to_error(errno);
				return false;
			}

			return true;
		}

		u64 read(void* buffer, u64 count) override
		{
			const auto result = ::read(m_fd, buffer, count);
			ensure(result != -1); // "file::read"

			return result;
		}

		u64 write(const void* buffer, u64 count) override
		{
			const auto result = ::write(m_fd, buffer, count);
			ensure(result != -1); // "file::write"

			return result;
		}

		u64 seek(s64 offset, seek_mode whence) override
		{
			if (whence > seek_end)
			{
				fmt::throw_exception("Invalid whence (0x%x)", whence);
			}

			const int mode =
				whence == seek_set ? SEEK_SET :
				whence == seek_cur ? SEEK_CUR : SEEK_END;

			const auto result = ::lseek(m_fd, offset, mode);

			if (result == -1)
			{
				g_tls_error = to_error(errno);
				return -1;
			}

			return result;
		}

		u64 size() override
		{
			struct ::stat file_info;
			ensure(::fstat(m_fd, &file_info) == 0); // "file::size"

			return file_info.st_size;
		}

		native_handle get_handle() override
		{
			return m_fd;
		}

		u64 write_gather(const iovec_clone* buffers, u64 buf_count) override
		{
			static_assert(sizeof(iovec) == sizeof(iovec_clone), "Weird iovec size");
			static_assert(offsetof(iovec, iov_len) == offsetof(iovec_clone, iov_len), "Weird iovec::iov_len offset");

			u64 result = 0;

			while (buf_count)
			{
				iovec arg[256];
				const auto count = std::min<u64>(buf_count, 256);
				std::memcpy(&arg, buffers, sizeof(iovec) * count);
				const auto added = ::writev(m_fd, arg, count);
				ensure(added != -1); // "file::write_gather"
				result += added;
				buf_count -= count;
				buffers += count;
			}

			return result;
		}
	};

	m_file = std::make_unique<unix_file>(fd);
#endif
}

fs::file::file(const void* ptr, usz size)
{
	class memory_stream : public file_base
	{
		u64 m_pos{};

		const char* const m_ptr;
		const u64 m_size;

	public:
		memory_stream(const void* ptr, u64 size)
			: m_ptr(static_cast<const char*>(ptr))
			, m_size(size)
		{
		}

		memory_stream(const memory_stream&) = delete;

		memory_stream& operator=(const memory_stream&) = delete;

		bool trunc(u64) override
		{
			return false;
		}

		u64 read(void* buffer, u64 count) override
		{
			if (m_pos < m_size)
			{
				// Get readable size
				if (const u64 result = std::min<u64>(count, m_size - m_pos))
				{
					std::memcpy(buffer, m_ptr + m_pos, result);
					m_pos += result;
					return result;
				}
			}

			return 0;
		}

		u64 write(const void*, u64) override
		{
			return 0;
		}

		u64 seek(s64 offset, fs::seek_mode whence) override
		{
			const s64 new_pos =
				whence == fs::seek_set ? offset :
				whence == fs::seek_cur ? offset + m_pos :
				whence == fs::seek_end ? offset + size() : -1;

			if (new_pos < 0)
			{
				fs::g_tls_error = fs::error::inval;
				return -1;
			}

			m_pos = new_pos;
			return m_pos;
		}

		u64 size() override
		{
			return m_size;
		}
	};

	m_file = std::make_unique<memory_stream>(ptr, size);
}

fs::native_handle fs::file::get_handle() const
{
	if (m_file)
	{
		return m_file->get_handle();
	}

#ifdef _WIN32
	return INVALID_HANDLE_VALUE;
#else
	return -1;
#endif
}

bool fs::dir::open(const std::string& path)
{
	if (path.empty())
	{
		// Don't allow opening empty path (TODO)
		g_tls_error = fs::error::noent;
		return false;
	}

	if (auto device = get_virtual_device(path))
	{
		if (auto&& _dir = device->open_dir(path))
		{
			m_dir = std::move(_dir);
			return true;
		}

		return false;
	}

#ifdef _WIN32
	WIN32_FIND_DATAW found;
	const auto handle = FindFirstFileExW(to_wchar(path + "/*").get(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_CASE_SENSITIVE | FIND_FIRST_EX_LARGE_FETCH);

	if (handle == INVALID_HANDLE_VALUE)
	{
		g_tls_error = to_error(GetLastError());
		return false;
	}

	class windows_dir final : public dir_base
	{
		std::vector<dir_entry> m_entries;
		usz m_pos = 0;

		void add_entry(const WIN32_FIND_DATAW& found)
		{
			dir_entry info;

			to_utf8(info.name, found.cFileName);
			info.is_directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			info.is_writable = (found.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0;
			info.size = ((u64)found.nFileSizeHigh << 32) | (u64)found.nFileSizeLow;
			info.atime = to_time(found.ftLastAccessTime);
			info.mtime = to_time(found.ftLastWriteTime);
			info.ctime = info.mtime;

			if (info.atime < info.mtime)
				info.atime = info.mtime;

			m_entries.emplace_back(std::move(info));
		}

	public:
		windows_dir(HANDLE handle, WIN32_FIND_DATAW& found)
		{
			add_entry(found);

			while (FindNextFileW(handle, &found))
			{
				add_entry(found);
			}

			ensure(ERROR_NO_MORE_FILES == GetLastError()); // "dir::read"
			FindClose(handle);
		}

		bool read(dir_entry& out) override
		{
			if (m_pos >= m_entries.size())
			{
				return false;
			}

			out = m_entries[m_pos++];
			return true;
		}

		void rewind() override
		{
			m_pos = 0;
		}
	};

	m_dir = std::make_unique<windows_dir>(handle, found);
#else
	::DIR* const ptr = ::opendir(path.c_str());

	if (!ptr)
	{
		g_tls_error = to_error(errno);
		return false;
	}

	class unix_dir final : public dir_base
	{
		::DIR* m_dd;

	public:
		unix_dir(::DIR* dd)
			: m_dd(dd)
		{
		}

		unix_dir(const unix_dir&) = delete;

		unix_dir& operator=(const unix_dir&) = delete;

		~unix_dir() override
		{
			::closedir(m_dd);
		}

		bool read(dir_entry& info) override
		{
			const auto found = ::readdir(m_dd);
			if (!found)
			{
				return false;
			}

			struct ::stat file_info;

			if (::fstatat(::dirfd(m_dd), found->d_name, &file_info, 0) != 0)
			{
				//failed metadata (broken symlink?), ignore and skip to next file
				return read(info);
			}

			info.name = found->d_name;
			info.is_directory = S_ISDIR(file_info.st_mode);
			info.is_writable = file_info.st_mode & 0200; // HACK: approximation
			info.size = file_info.st_size;
			info.atime = file_info.st_atime;
			info.mtime = file_info.st_mtime;
			info.ctime = info.mtime;

			if (info.atime < info.mtime)
				info.atime = info.mtime;

			return true;
		}

		void rewind() override
		{
			::rewinddir(m_dd);
		}
	};

	m_dir = std::make_unique<unix_dir>(ptr);
#endif

	return true;
}

bool fs::file::strict_read_check(u64 _size, u64 type_size) const
{
	if (usz pos0 = pos(), size0 = size(); pos0 >= size0 || (size0 - pos0) / type_size < _size)
	{
		fs::g_tls_error = fs::error::inval;
		return false;
	}

	return true;
}

const std::string& fs::get_config_dir()
{
	// Use magic static
	static const std::string s_dir = []
	{
		std::string dir;

#ifdef _WIN32
		wchar_t buf[32768];
		constexpr DWORD size = static_cast<DWORD>(std::size(buf));
		if (GetEnvironmentVariable(L"RPCS3_CONFIG_DIR", buf, size) - 1 >= size - 1 &&
			GetModuleFileName(nullptr, buf, size) - 1 >= size - 1)
		{
			MessageBoxA(nullptr, fmt::format("GetModuleFileName() failed: error %u.", GetLastError()).c_str(), "fs::get_config_dir()", MB_ICONERROR);
			return dir; // empty
		}

		to_utf8(dir, buf); // Convert to UTF-8

		std::replace(dir.begin(), dir.end(), '\\', '/');

		dir.resize(dir.rfind('/') + 1);
#else

#ifdef __APPLE__
		if (const char* home = ::getenv("HOME"))
			dir = home + "/Library/Application Support"s;
#else
		if (const char* conf = ::getenv("XDG_CONFIG_HOME"))
			dir = conf;
		else if (const char* home = ::getenv("HOME"))
			dir = home + "/.config"s;
#endif
		else // Just in case
			dir = "./config";

		dir += "/rpcs3/";

		if (!create_path(dir))
		{
			std::printf("Failed to create configuration directory '%s' (%d).\n", dir.c_str(), errno);
		}
#endif

		return dir;
	}();

	return s_dir;
}

const std::string& fs::get_cache_dir()
{
	static const std::string s_dir = []
	{
		std::string dir;

#ifdef _WIN32
		dir = get_config_dir();
#else

#ifdef __APPLE__
		if (const char* home = ::getenv("HOME"))
			dir = home + "/Library/Caches"s;
#else
		if (const char* cache = ::getenv("XDG_CACHE_HOME"))
			dir = cache;
		else if (const char* conf = ::getenv("XDG_CONFIG_HOME"))
			dir = conf;
		else if (const char* home = ::getenv("HOME"))
			dir = home + "/.cache"s;
#endif
		else // Just in case
			dir = "./cache";

		dir += "/rpcs3/";

		if (!create_path(dir))
		{
			std::printf("Failed to create configuration directory '%s' (%d).\n", dir.c_str(), errno);
		}
#endif

		return dir;
	}();

	return s_dir;
}

bool fs::remove_all(const std::string& path, bool remove_root)
{
	if (const auto root_dir = dir(path))
	{
		for (const auto& entry : root_dir)
		{
			if (entry.name == "." || entry.name == "..")
			{
				continue;
			}

			if (!entry.is_directory)
			{
				if (!remove_file(path_append(path, entry.name)))
				{
					return false;
				}
			}
			else
			{
				if (!remove_all(path_append(path, entry.name)))
				{
					return false;
				}
			}
		}
	}
	else
	{
		return false;
	}

	if (remove_root)
	{
		return remove_dir(path);
	}

	return true;
}

u64 fs::get_dir_size(const std::string& path, u64 rounding_alignment)
{
	u64 result = 0;

	const auto root_dir = dir(path);

	if (!root_dir)
	{
		return -1;
	}

	for (const auto& entry : root_dir)
	{
		if (entry.name == "." || entry.name == "..")
		{
			continue;
		}

		if (!entry.is_directory)
		{
			result += utils::align(entry.size, rounding_alignment);
		}
		else
		{
			const u64 size = get_dir_size(path_append(path, entry.name), rounding_alignment);

			if (size == umax)
			{
				return size;
			}

			result += size;
		}
	}

	return result;
}

fs::file fs::make_gather(std::vector<fs::file> files)
{
	struct gather_stream : file_base
	{
		u64 pos = 0;
		u64 end = 0;
		std::vector<file> files{};
		std::map<u64, u64> ends{}; // Fragment End Offset -> Index

		gather_stream(std::vector<fs::file> arg)
			: files(std::move(arg))
		{
			// Preprocess files
			for (auto&& f : files)
			{
				end += f.size();
				ends.emplace(end, ends.size());
			}
		}

		~gather_stream() override
		{
		}

		fs::stat_t stat() override
		{
			fs::stat_t result{};

			if (!files.empty())
			{
				result = files[0].stat();
			}

			result.is_directory = false;
			result.is_writable = false;
			result.size = end;
			return result;
		}

		bool trunc(u64) override
		{
			return false;
		}

		u64 read(void* buffer, u64 size) override
		{
			if (pos < end)
			{
				// Current pos
				const u64 start = pos;

				// Get readable size
				if (const u64 max = std::min<u64>(size, end - pos))
				{
					u8* buf_out = static_cast<u8*>(buffer);
					u64 buf_max = max;

					for (auto it = ends.upper_bound(pos); it != ends.end(); ++it)
					{
						// Set position for the fragment
						files[it->second].seek(pos - it->first, fs::seek_end);

						const u64 count = std::min<u64>(it->first - pos, buf_max);
						const u64 read  = files[it->second].read(buf_out, count);

						buf_out += count;
						buf_max -= count;
						pos     += read;

						if (read < count || buf_max == 0)
						{
							break;
						}
					}

					return pos - start;
				}
			}

			return 0;
		}

		u64 write(const void*, u64) override
		{
			return 0;
		}

		u64 seek(s64 offset, seek_mode whence) override
		{
			const s64 new_pos =
				whence == fs::seek_set ? offset :
				whence == fs::seek_cur ? offset + pos :
				whence == fs::seek_end ? offset + end : -1;

			if (new_pos < 0)
			{
				fs::g_tls_error = fs::error::inval;
				return -1;
			}

			pos = new_pos;
			return pos;
		}

		u64 size() override
		{
			return end;
		}
	};

	fs::file result;
	result.reset(std::make_unique<gather_stream>(std::move(files)));
	return result;
}

fs::pending_file::pending_file(const std::string& path)
{
	do
	{
		m_path = fmt::format(u8"%s/＄%s.%s.tmp", get_parent_dir(path), std::string_view(path).substr(path.find_last_of(fs::delim) + 1), fmt::base57(utils::get_unique_tsc()));

		if (file.open(m_path, fs::create + fs::write + fs::read + fs::excl))
		{
			m_dest = path;
			break;
		}

		m_path.clear();
	}
	while (fs::g_tls_error == fs::error::exist); // Only retry if failed due to existing file
}

fs::pending_file::~pending_file()
{
	file.close();

	if (!m_path.empty())
	{
		fs::remove_file(m_path);
	}
}

bool fs::pending_file::commit(bool overwrite)
{
	if (!file || m_path.empty())
	{
		fs::g_tls_error = fs::error::noent;
		return false;
	}

	// The temporary file's contents must be on disk before rename
	file.sync();
	file.close();

	if (fs::rename(m_path, m_dest, overwrite))
	{
		// Disable the destructor
		m_path.clear();
		return true;
	}

	return false;
}

template<>
void fmt_class_string<fs::seek_mode>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto arg)
	{
		switch (arg)
		{
		STR_CASE(fs::seek_mode::seek_set);
		STR_CASE(fs::seek_mode::seek_cur);
		STR_CASE(fs::seek_mode::seek_end);
		}

		return unknown;
	});
}

template<>
void fmt_class_string<fs::error>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto arg)
	{
		switch (arg)
		{
		case fs::error::ok: return "OK";

		case fs::error::inval: return "Invalid arguments";
		case fs::error::noent: return "Not found";
		case fs::error::exist: return "Already exists";
		case fs::error::acces: return "Access violation";
		case fs::error::notempty: return "Not empty";
		case fs::error::readonly: return "Read only";
		case fs::error::isdir: return "Is a directory";
		case fs::error::toolong: return "Path too long";
		case fs::error::nospace: return "Not enough space on the device";
		case fs::error::unknown: return "Unknown system error";
		}

		return unknown;
	});
}
