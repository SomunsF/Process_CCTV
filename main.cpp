#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <ctime>
#include <deque>  // 滑动窗口
#include <cmath>  // 计算标准差
#include <unordered_map>
#include <numeric>

#include <csignal>  // 用于 kill, SIGSTOP, SIGCONT

// POSIX 相关
#include <dirent.h>
#include <unistd.h>

// ncurses 库
#include <ncurses.h>

// Socket 相关
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;


// ------------------------
// 数据结构及全局变量
// ------------------------
struct ProcessInfo {
    int pid;
    string name;
    double cpu_usage; // 百分比
    double mem_usage; // 百分比
};

struct ProcessStatsHistory {
    deque<double> cpu_history;
    deque<double> mem_history;
};
unordered_map<int, ProcessStatsHistory> g_processHistory;

// 全局共享变量
atomic<long> g_prevTotalCpuTime(0); // 原子变量确保多线程安全
vector<ProcessInfo> g_processList;
mutex g_mutex;
atomic<bool> g_running(true);
string g_filter = ""; // 若不为空则仅显示名称包含此子串的进程

// 上一次采样时保存的 CPU 时间数据
struct CpuTimes {
    long utime;
    long stime;
};
map<int, CpuTimes> prevCpuTimes;

// ------------------------
// 辅助函数
// ------------------------

// 从 /proc/meminfo 获取总内存(KB)
long get_total_memory() {
    ifstream meminfo("/proc/meminfo");
    string line;
    long total_mem = 0;
    while(getline(meminfo, line)) {
        if(line.find("MemTotal:") == 0) {
            istringstream iss(line);
            string label, unit;
            long mem;
            iss >> label >> mem >> unit;
            total_mem = mem;
            break;
        }
    }
    return total_mem;
}

// 获取 /proc 中所有进程 PID
vector<int> get_all_pids() {
    vector<int> pids;
    DIR* dir = opendir("/proc");
    if(!dir) return pids;
    struct dirent* entry;
    while((entry = readdir(dir)) != nullptr) {
        if(entry->d_type == DT_DIR) {
            string dir_name = entry->d_name;
            if(all_of(dir_name.begin(), dir_name.end(), ::isdigit)) {
                try {
                    int pid = stoi(dir_name);
                    pids.push_back(pid);
                } catch(...) {
                    // 转换错误则忽略
                }
            }
        }
    }
    closedir(dir);
    return pids;
}

// 通过 /proc/[pid]/comm 获取进程名称
string get_process_name(int pid) {
    string path = "/proc/" + to_string(pid) + "/comm";
    ifstream file(path);
    string name;
    getline(file, name);
    return name;
}

// 从 /proc/[pid]/stat 获取 CPU 时间（utime, stime）
pair<long, long> get_process_cpu_time(int pid) {
    string path = "/proc/" + to_string(pid) + "/stat";
    ifstream file(path);
    string line;
    getline(file, line);
    istringstream iss(line);
    string token;
    // 先读 pid 和 comm 两个字段
    iss >> token; // PID
    string comm;
    iss >> comm;  // 进程名称（带括号）
    // 跳过后面 11 个字段，utime 为第 14 字段，stime 为第 15 字段
    for (int i = 0; i < 11; ++i)
        iss >> token;
    long utime = 0, stime = 0;
    iss >> utime >> stime;
    return {utime, stime};
}

// 获取 /proc/stat 第一行的总 CPU 时间
long get_total_cpu_time() {
    ifstream file("/proc/stat");
    string line;
    getline(file, line);
    istringstream iss(line);
    string cpu;
    long total = 0, value;
    iss >> cpu; // "cpu"
    while(iss >> value) {
        total += value;
    }
    return total;
}

// 从 /proc/[pid]/status 中读取 VmRSS（单位 KB）
long get_process_memory(int pid) {
    string path = "/proc/" + to_string(pid) + "/status";
    ifstream file(path);
    string line;
    while(getline(file, line)) {
        if(line.find("VmRSS:") == 0) {
            istringstream iss(line);
            string label, unit;
            long mem;
            iss >> label >> mem >> unit;
            return mem;
        }
    }
    return 0;
}

// 生成简单的 CPU 使用柱状图（基于百分比，最大长度 maxWidth）
// 在这里对 usage 进行范围限定，防止负值或超过 100 的情况
string generate_bar(double usage, int maxWidth = 20) {
    if (usage < 0) usage = 0;
    if (usage > 100) usage = 100;
    int length = static_cast<int>((usage / 100.0) * maxWidth);
    return string(length, '#');
}

// ------------------------
// 数据更新与日志记录
// ------------------------

// 更新进程信息数据，计算 CPU 与内存使用率
void update_process_data() {
    long total_memory = get_total_memory();
    long current_total_cpu_time = get_total_cpu_time();
    long delta_total_cpu_time = 0;

    // 计算总CPU时间增量
    long previous_total = g_prevTotalCpuTime.load();
    if (previous_total != 0) {
        delta_total_cpu_time = current_total_cpu_time - previous_total;
    }
    // 更新全局的总CPU时间以便下次计算
    g_prevTotalCpuTime.store(current_total_cpu_time);

    vector<int> pids = get_all_pids();
    vector<ProcessInfo> newList;

    for (int pid : pids) {
        string name = get_process_name(pid);
        // 应用过滤条件：过滤不匹配的进程
        if (!g_filter.empty()) {
            if (name.find(g_filter) == string::npos)
                continue;
        }
        auto cpu_times = get_process_cpu_time(pid);
        long proc_time = cpu_times.first + cpu_times.second;
        double cpu_usage = 0.0;
        // 检查之前是否有数据，并且确保delta_total_cpu_time有效
        if (prevCpuTimes.find(pid) != prevCpuTimes.end() && delta_total_cpu_time > 0) {
            long prev_proc_time = prevCpuTimes[pid].utime + prevCpuTimes[pid].stime;
            long delta_proc_time = proc_time - prev_proc_time;
            // 计算该进程在采样间隔内占用的CPU百分比
            cpu_usage = 100.0 * delta_proc_time / delta_total_cpu_time;
            if(cpu_usage > 100) cpu_usage = 100; // 限定最大值
        }
        prevCpuTimes[pid] = {cpu_times.first, cpu_times.second};

        long mem = get_process_memory(pid);
        double mem_usage = total_memory > 0 ? (100.0 * mem / total_memory) : 0.0;

        ProcessInfo info {pid, name, cpu_usage, mem_usage};
        newList.push_back(info);
    }
    
    // 按 CPU 使用率降序排序
    sort(newList.begin(), newList.end(), [](const ProcessInfo &a, const ProcessInfo &b) {
        return a.cpu_usage > b.cpu_usage;
    });
    
    {
        lock_guard<mutex> lock(g_mutex);
        g_processList = newList;
    }
            // 记录历史并进行简单异常检测
            for (const auto& proc : newList) {
                auto& history = g_processHistory[proc.pid];
    
                // 限定窗口大小为 10
                if (history.cpu_history.size() >= 10)
                    history.cpu_history.pop_front();
                if (history.mem_history.size() >= 10)
                    history.mem_history.pop_front();
    
                history.cpu_history.push_back(proc.cpu_usage);
                history.mem_history.push_back(proc.mem_usage);
            }
    
}


// 定时更新进程数据的线程函数，默认每 1000ms 更新一次
void process_monitor_updater(int interval_ms = 1000) {
    while(g_running.load()) {
        update_process_data();
        this_thread::sleep_for(chrono::milliseconds(interval_ms));
    }
}

// 日志记录线程：每5秒将当前进程数据写入日志文件
void process_logger(int interval_ms = 5000 ) {
    while(g_running.load()) {
        {
            lock_guard<mutex> lock(g_mutex);
            ofstream logFile("process_monitor.log", ios::app);
            if (logFile.is_open()) {
                time_t now = time(nullptr);
                logFile << "=== " << ctime(&now);
                for (auto &proc : g_processList) {
                    logFile << "PID: " << proc.pid 
                            << " | Name: " << proc.name 
                            << " | CPU: " << fixed << setprecision(2) << proc.cpu_usage << "%"
                            << " | MEM: " << fixed << setprecision(2) << proc.mem_usage << "%" 
                            << "\n";
                }
                logFile << "\n";
                logFile.close();
            }
        }
        this_thread::sleep_for(chrono::milliseconds(interval_ms));
    }
}

// ------------------------
// HTTP 服务器功能
// ------------------------

// 生成当前进程列表的 JSON 字符串
string generate_json() {
    stringstream ss;
    ss << "[";
    bool first = true;
    vector<ProcessInfo> snapshot;
    {
        lock_guard<mutex> lock(g_mutex);
        snapshot = g_processList;
    }
    for (auto &proc : snapshot) {
        if (!first)
            ss << ",";
        first = false;
        ss << "{";
        ss << "\"pid\":" << proc.pid << ",";
        ss << "\"name\":\"" << proc.name << "\",";
        ss << "\"cpu_usage\":" << proc.cpu_usage << ",";
        ss << "\"mem_usage\":" << proc.mem_usage;
        ss << "}";
    }
    ss << "]";
    return ss.str();
}

// 一个简易 HTTP 服务器线程（监听8080端口），支持 GET /processes 返回 JSON 数据
// 修改 http_server 为非阻塞版本
void http_server(int port = 8080) {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        return;
    }
    
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        return;
    }
    
    // 设置 socket 为非阻塞
    fcntl(server_fd, F_SETFL, O_NONBLOCK);
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        return;
    }
    
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        close(server_fd);
        return;
    }
    
    // 可选：移除启动信息
    // cout << "HTTP Server running on port " << port << endl;
    
    while(g_running.load()) {
        // 设置一个超时时间来等待连接
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int activity = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            if(g_running.load())
                perror("select error");
            break;
        } else if (activity == 0) {
            // 超时后继续循环检查 g_running 状态
            continue;
        }
        
        if (FD_ISSET(server_fd, &readfds)) {
            client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
            if (client_socket < 0) {
                // 非阻塞模式下，可能会返回 EWOULDBLOCK/EAGAIN，不必报错
                if(errno != EWOULDBLOCK && errno != EAGAIN)
                    perror("accept");
                continue;
            }
            
            char buffer[1024] = {0};
            read(client_socket, buffer, 1024);
            string request(buffer);
            if (request.find("GET /processes") != string::npos) {
                string json = generate_json();
                string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " 
                                  + to_string(json.size()) + "\r\n\r\n" + json;
                send(client_socket, response.c_str(), response.size(), 0);
            } else {
                string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                send(client_socket, response.c_str(), response.size(), 0);
            }
            close(client_socket);
        }
    }
    close(server_fd);
}
// 简单的异常检测函数：当前值是否明显偏离历史均值（滑动窗口标准差法）
bool is_anomalous(const deque<double>& history, double current) {
    if (history.size() < 5) return false;
    double mean = accumulate(history.begin(), history.end(), 0.0) / history.size();
    double sq_sum = 0.0;
    for (auto val : history) {
        sq_sum += (val - mean) * (val - mean);
    }
    double stdev = sqrt(sq_sum / history.size());
    return fabs(current - mean) > 2 * stdev;
}


// ------------------------
// ncurses 交互界面
// ------------------------

/*
   在 ncurses 界面中，除了实时显示进程信息外，还支持：
   - 按 'q' 键退出程序
   - 按 'k' 键：提示输入 PID 后发送 SIGTERM（终止进程）
   - 按 's' 键：提示输入 PID 后发送 SIGSTOP（暂停进程）
   - 按 'r' 键：提示输入 PID 后发送 SIGCONT（恢复进程）
*/
void curses_ui() {
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);  // 非阻塞
    curs_set(0);
    
    while(g_running.load()) {
        clear();
        vector<ProcessInfo> snapshot;
        {
            lock_guard<mutex> lock(g_mutex);
            snapshot = g_processList;
        }
        
        int row = 0;
        mvprintw(row++, 0, "Process Monitor (Filter: %s)", g_filter.c_str());
        mvprintw(row++, 0, "Press 'q' to quit, 'k': kill, 's': suspend, 'r': resume.");
        mvprintw(row++, 0, "%-8s %-20s %-10s %-10s %s", "PID", "Name", "CPU (%)", "MEM (%)", "CPU Bar");
        mvprintw(row++, 0, "%s", string(70, '-').c_str());
        for (auto &proc : snapshot) {
            string bar = generate_bar(proc.cpu_usage, 20);
            mvprintw(row++, 0, "%-8d %-20s %-10.2f %-10.2f %s", proc.pid, proc.name.c_str(), proc.cpu_usage, proc.mem_usage, bar.c_str());
            if (row > LINES - 3) break;
        }
        refresh();
                // 显示检测到的异常进程
                int anomaly_row = row + 1;
                int count = 0;
                mvprintw(anomaly_row++, 0, "[Anomalies Detected]:");
                for (auto& proc : snapshot) {
                    auto it = g_processHistory.find(proc.pid);
                    if (it != g_processHistory.end()) {
                        bool cpu_abnormal = is_anomalous(it->second.cpu_history, proc.cpu_usage);
                        bool mem_abnormal = is_anomalous(it->second.mem_history, proc.mem_usage);
                        if (cpu_abnormal || mem_abnormal) {
                            mvprintw(anomaly_row++, 0, "PID %d (%s) - Abnormal %s%s", proc.pid, proc.name.c_str(),
                                cpu_abnormal ? "CPU " : "", mem_abnormal ? "MEM" : "");
                            if (++count >= 5) break;
                        }
                    }
                }
        
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            g_running.store(false);
            break;
        }
        // 辅助 lambda，提示用户输入 PID 并发送指定信号
        auto prompt_and_send_signal = [ch](int sig, const char* action) {
            nodelay(stdscr, FALSE);  // 暂停非阻塞
            echo();
            char input[16] = {0};
            mvprintw(LINES-2, 0, "Enter PID to %s: ", action);
            getnstr(input, 15);
            int pid = atoi(input);
            if (pid > 0) {
                if (kill(pid, sig) == 0) {
                    mvprintw(LINES-1, 0, "Signal sent to process %d successfully.", pid);
                } else {
                    mvprintw(LINES-1, 0, "Failed to send signal to process %d.", pid);
                }
            } else {
                mvprintw(LINES-1, 0, "Invalid PID input.");
            }
            noecho();
            nodelay(stdscr, TRUE);
            this_thread::sleep_for(chrono::milliseconds(1000));
        };
        
        if (ch == 'k' || ch == 'K') {
            prompt_and_send_signal(SIGTERM, "kill");
        } else if (ch == 's' || ch == 'S') {
            prompt_and_send_signal(SIGSTOP, "suspend");
        } else if (ch == 'r' || ch == 'R') {
            prompt_and_send_signal(SIGCONT, "resume");
        }
        
        this_thread::sleep_for(chrono::milliseconds(500));
    }
    endwin();
}

// ------------------------
// 使用说明打印
// ------------------------
void usage(const char* prog) {
    cout << "Usage: " << prog << " [-f filter]" << endl;
    cout << "   -f filter : Filter processes by name substring." << endl;
}
// 线性回归预测函数
// 线性回归预测函数（续尾）
double linear_predict(const deque<double>& data) {
    int n = data.size();
    if (n < 2) return data.empty() ? 0.0 : data.back();

    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    for (int i = 0; i < n; ++i) {
        sum_x += i;
        sum_y += data[i];
        sum_xy += i * data[i];
        sum_xx += i * i;
    }
    double slope = (n * sum_xy - sum_x * sum_y) / (n * sum_xx - sum_x * sum_x + 1e-6);
    double intercept = (sum_y - slope * sum_x) / n;
    return slope * n + intercept;  // 预测下一时刻
}



// ------------------------
// 主程序入口
// ------------------------
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (string(argv[i]) == "-f" && i + 1 < argc) {
            g_filter = argv[i + 1];
            ++i;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    thread updater(process_monitor_updater, 1000); // 数据更新线程 500ms
    thread logger(process_logger, 5000);          // 日志记录线程 5000ms
    thread http(http_server, 8080);                // HTTP JSON API

    curses_ui(); // 主界面（阻塞运行）

    // 程序结束时收尾
    updater.join();
    logger.join();
    http.join();

    return 0;
}
