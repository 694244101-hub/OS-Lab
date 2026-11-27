// system_info.cpp
// Cross-platform system information viewer (Linux / Windows)
// Build (Linux)   : g++ -std=c++17 -O2 system_info.cpp -o system_info
// Build (Windows) : cl /EHsc /std:c++17 system_info.cpp
//                   g++ -std=c++17 system_info.cpp -o system_info.exe   (MinGW)

#define WIN32_LEAN_AND_MEAN

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <tlhelp32.h>
#else
#  include <dirent.h>
#  include <mntent.h>
#  include <sys/stat.h>
#  include <sys/statvfs.h>
#  include <sys/sysinfo.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#endif

namespace {

// ---------- Utility ----------

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string formatBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int i = 0;
    while (value >= 1024.0 && i < 5) {
        value /= 1024.0;
        ++i;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(i == 0 ? 0 : 2)
        << value << ' ' << units[i];
    return oss.str();
}

void printHeader(const std::string& title) {
    std::cout << "\n=== " << title << " ===\n";
}

std::string unquote(std::string value) {
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

// Read "key=value" style files such as /etc/os-release
std::string getConfigValue(const std::string& file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        if (trim(line.substr(0, pos)) == key) {
            return unquote(trim(line.substr(pos + 1)));
        }
    }
    return {};
}

uint64_t memInfoValueKB(const std::string& key) {
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key + ":", 0) == 0) {
            std::istringstream iss(line.substr(key.size() + 1));
            uint64_t value = 0;
            iss >> value;
            return value;
        }
    }
    return 0;
}

struct ProcessInfo {
    unsigned long pid = 0;
    std::string name;
    std::string state;
    uint64_t rss = 0;
};

struct MemoryInfo {
    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t available = 0;
};

struct OsInfo {
    std::string name;
    std::string kernel;
};

struct DiskInfo {
    std::string mount;
    std::string type;
    std::string label;
    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t available = 0;
};

// ---------- 1. CPU ----------

std::string getCpuName() {
#ifdef _WIN32
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        char buffer[512] = {};
        DWORD size = sizeof(buffer);
        std::string result;
        if (RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
            result = trim(buffer);
        }
        RegCloseKey(key);
        if (!result.empty()) return result;
    }
    return "Unknown";
#else
    std::ifstream in("/proc/cpuinfo");
    std::string line, result, fallback;
    while (std::getline(in, line)) {
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        const auto key = trim(line.substr(0, pos));
        const auto value = trim(line.substr(pos + 1));
        if (value.empty()) continue;
        if (key == "model name") return value;   // x86
        if (key == "Hardware") fallback = value; // ARM
        if (key == "Processor" && fallback.empty()) fallback = value;
    }
    return fallback.empty() ? "Unknown" : fallback;
#endif
}

// ---------- 2. Memory ----------

MemoryInfo getMemory() {
    MemoryInfo info;
#ifdef _WIN32
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        info.total = status.ullTotalPhys;
        info.available = status.ullAvailPhys;
    }
#else
    info.total = memInfoValueKB("MemTotal") * 1024ULL;
    info.available = memInfoValueKB("MemAvailable") * 1024ULL;
    if (info.available == 0) {
        info.available = (memInfoValueKB("MemFree") + memInfoValueKB("Buffers") +
                          memInfoValueKB("Cached")) * 1024ULL;
    }
    if (info.total == 0) {
        struct sysinfo si = {};
        if (sysinfo(&si) == 0) {
            info.total = static_cast<uint64_t>(si.totalram) * si.mem_unit;
            info.available = static_cast<uint64_t>(si.freeram) * si.mem_unit;
        }
    }
#endif
    info.used = info.total > info.available ? info.total - info.available : 0;
    return info;
}

// ---------- 3. Operating system ----------

OsInfo getOsInfo() {
    OsInfo info;
#ifdef _WIN32
    auto regString = [](const char* subKey, const char* valueName) -> std::string {
        HKEY key = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subKey, 0,
                          KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
            return {};
        }
        char buffer[512] = {};
        DWORD size = sizeof(buffer);
        std::string result;
        if (RegQueryValueExA(key, valueName, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
            result = trim(buffer);
        }
        RegCloseKey(key);
        return result;
    };

    const char* subKey = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = regString(subKey, "ProductName");
    std::string version = regString(subKey, "DisplayVersion");
    if (version.empty()) version = regString(subKey, "CurrentVersion");
    std::string build = regString(subKey, "CurrentBuild");

    info.name = product.empty() ? "Unknown Windows" : product;
    if (!version.empty()) info.name += " " + version;
    if (!build.empty()) info.name += " (build " + build + ")";
    info.kernel = "Windows NT";
#else
    info.name = getConfigValue("/etc/os-release", "PRETTY_NAME");
    if (info.name.empty()) {
        info.name = getConfigValue("/etc/lsb-release", "DISTRIB_DESCRIPTION");
    }
    if (info.name.empty()) info.name = "Unknown Linux distribution";

    struct utsname uts = {};
    if (uname(&uts) == 0) {
        info.kernel = std::string(uts.sysname) + " " + uts.release +
                      " (" + uts.machine + ")";
    }
#endif
    return info;
}

// ---------- 4. Storage ----------

std::vector<DiskInfo> getDisks() {
    std::vector<DiskInfo> disks;

#ifdef _WIN32
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;

        std::string root;
        root += static_cast<char>('A' + i);
        root += ":\\";

        const UINT driveType = GetDriveTypeA(root.c_str());
        if (driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_CDROM) continue;

        ULARGE_INTEGER available, total, totalFree;
        if (!GetDiskFreeSpaceExA(root.c_str(), &available, &total, &totalFree)) continue;

        DiskInfo disk;
        disk.mount = root;
        disk.type = "local";
        disk.total = total.QuadPart;
        disk.available = available.QuadPart;
        disk.used = disk.total > totalFree.QuadPart
                        ? disk.total - totalFree.QuadPart : 0;

        char label[MAX_PATH] = {};
        char fs[MAX_PATH] = {};
        if (GetVolumeInformationA(root.c_str(), label, MAX_PATH, nullptr, nullptr,
                                  nullptr, fs, MAX_PATH)) {
            disk.label = trim(label);
            disk.type = trim(fs);
        }
        disks.push_back(disk);
    }
#else
    static const std::set<std::string> pseudoFs = {
        "proc", "sysfs", "devtmpfs", "tmpfs", "devpts", "securityfs",
        "cgroup", "cgroup2", "pstore", "hugetlbfs", "mqueue", "debugfs",
        "tracefs", "fusectl", "configfs", "binfmt_misc", "nsfs", "bpf",
        "autofs", "rpc_pipefs", "nfsd"
    };

    std::set<dev_t> seenDevices;
    FILE* mounts = setmntent("/proc/mounts", "r");
    if (mounts != nullptr) {
        struct mntent* entry = nullptr;
        while ((entry = getmntent(mounts)) != nullptr) {
            if (pseudoFs.count(entry->mnt_type) != 0) continue;

            const std::string mountPoint = entry->mnt_dir;
            struct stat status = {};
            // Skip bind mounts of individual files, deduplicate bind mounts
            // that point to the same underlying filesystem.
            if (stat(mountPoint.c_str(), &status) != 0 ||
                !S_ISDIR(status.st_mode) ||
                !seenDevices.insert(status.st_dev).second) {
                continue;
            }

            struct statvfs vfs = {};
            if (statvfs(mountPoint.c_str(), &vfs) != 0) continue;

            const uint64_t blockSize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
            DiskInfo disk;
            disk.mount = mountPoint;
            disk.type = entry->mnt_type;
            disk.total = static_cast<uint64_t>(vfs.f_blocks) * blockSize;
            const uint64_t freeBytes = static_cast<uint64_t>(vfs.f_bfree) * blockSize;
            disk.available = static_cast<uint64_t>(vfs.f_bavail) * blockSize;
            disk.used = disk.total > freeBytes ? disk.total - freeBytes : 0;
            disks.push_back(disk);
        }
        endmntent(mounts);
    }

    if (disks.empty()) {
        struct statvfs vfs = {};
        if (statvfs("/", &vfs) == 0) {
            const uint64_t blockSize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
            DiskInfo disk;
            disk.mount = "/";
            disk.type = "unknown";
            disk.total = static_cast<uint64_t>(vfs.f_blocks) * blockSize;
            const uint64_t freeBytes = static_cast<uint64_t>(vfs.f_bfree) * blockSize;
            disk.available = static_cast<uint64_t>(vfs.f_bavail) * blockSize;
            disk.used = disk.total > freeBytes ? disk.total - freeBytes : 0;
            disks.push_back(disk);
        }
    }
#endif
    return disks;
}

// ---------- 5. Processes ----------

std::vector<ProcessInfo> getProcesses() {
    std::vector<ProcessInfo> processes;

#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processes;

    PROCESSENTRY32A entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstA(snapshot, &entry)) {
        do {
            ProcessInfo process;
            process.pid = entry.th32ProcessID;
            process.name = entry.szExeFile;
            process.state = "-";
            processes.push_back(process);
        } while (Process32NextA(snapshot, &entry));
    }
    CloseHandle(snapshot);
#else
    DIR* directory = opendir("/proc");
    if (directory == nullptr) return processes;

    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.empty()) continue;
        if (!std::all_of(name.begin(), name.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }

        ProcessInfo process;
        process.pid = std::strtoul(name.c_str(), nullptr, 10);

        std::ifstream comm("/proc/" + name + "/comm");
        if (comm) std::getline(comm, process.name);
        if (process.name.empty()) process.name = "<unknown>";

        std::ifstream status("/proc/" + name + "/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("State:", 0) == 0) {
                process.state = trim(line.substr(6));
            } else if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream iss(line.substr(6));
                uint64_t kilobytes = 0;
                iss >> kilobytes;
                process.rss = kilobytes * 1024ULL;
            }
        }
        processes.push_back(process);
    }
    closedir(directory);
#endif

    std::sort(processes.begin(), processes.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  return a.pid < b.pid;
              });
    return processes;
}

} // namespace

int main() {
    std::cout << "System Information Report\n"
              << std::string(40, '=') << '\n';

    // 1. CPU
    printHeader("1. CPU");
    std::cout << "Name : " << getCpuName() << '\n';

    // 2. Memory
    printHeader("2. Memory (RAM)");
    const MemoryInfo memory = getMemory();
    std::cout << "Total     : " << formatBytes(memory.total) << '\n';
    std::cout << "Used      : " << formatBytes(memory.used) << '\n';
    std::cout << "Available : " << formatBytes(memory.available) << '\n';
    if (memory.total > 0) {
        std::cout << "Usage     : " << std::fixed << std::setprecision(1)
                  << (memory.used * 100.0 / memory.total) << "%\n";
    }

    // 3. OS
    printHeader("3. Operating System");
    const OsInfo os = getOsInfo();
    std::cout << "Name   : " << os.name << '\n';
    std::cout << "Kernel : " << os.kernel << '\n';

    // 4. Storage
    printHeader("4. Storage");
    const std::vector<DiskInfo> disks = getDisks();
    if (disks.empty()) {
        std::cout << "No storage device found.\n";
    }
    for (const auto& disk : disks) {
        std::cout << "Mount : " << disk.mount;
        if (!disk.label.empty()) std::cout << " [" << disk.label << "]";
        if (!disk.type.empty()) std::cout << " (" << disk.type << ")";
        std::cout << '\n';
        std::cout << "  Total     : " << formatBytes(disk.total) << '\n';
        std::cout << "  Used      : " << formatBytes(disk.used) << '\n';
        std::cout << "  Available : " << formatBytes(disk.available) << '\n';
        if (disk.total > 0) {
            std::cout << "  Usage     : " << std::fixed << std::setprecision(1)
                      << (disk.used * 100.0 / disk.total) << "%\n";
        }
    }

    // 5. Running processes
    printHeader("5. Running Processes");
    const std::vector<ProcessInfo> processes = getProcesses();
    std::cout << "Count : " << processes.size() << '\n';
    std::cout << std::left << std::setw(10) << "PID"
              << std::setw(32) << "Name"
              << std::setw(17) << "State"
              << "RSS\n"
              << std::string(69, '-') << '\n';
    for (const auto& process : processes) {
        const std::string name = process.name.size() > 31
                                     ? process.name.substr(0, 31) : process.name;
        std::cout << std::left << std::setw(10) << process.pid
                  << std::setw(32) << name
                  << std::setw(17) << process.state
                  << (process.rss ? formatBytes(process.rss) : "-") << '\n';
    }

    return 0;
}
