#include "core/file_util.h"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace gvr {

bool writeFileAtomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0) { ::close(fd); std::remove(tmp.c_str()); return false; }
        done += size_t(n);
    }
    // On the storage before the rename: a power cut can't leave the new name on an empty file.
    if (::fsync(fd) != 0) { ::close(fd); std::remove(tmp.c_str()); return false; }
    if (::close(fd) != 0) { std::remove(tmp.c_str()); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    return true;
}

}  // namespace gvr
