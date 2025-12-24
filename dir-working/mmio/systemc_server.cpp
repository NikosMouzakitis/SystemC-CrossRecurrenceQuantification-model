// systemc_server_final.cpp - COMPLETE SELF-CONTAINED
#include <systemc>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <csignal>

using namespace std;
using namespace sc_core;

#define SOCKET_PATH "/tmp/crqa_socket"
#define N_SAMPLES 512

// Message structures (MUST match QEMU exactly)
#pragma pack(push, 1)
struct Input {
    double R;
    double sig1[N_SAMPLES];
    double sig2[N_SAMPLES];
    int32_t opcode;
    int32_t ready;
};

struct Output {
    double eps, rr, det, l, lmax, div, ent, lam;
};
#pragma pack(pop)

// CRQA computation function
void compute_crqa_complete(double R, double* sig1, double* sig2, double results[8]) {
    // 1. Normalize
    double mean1 = 0, mean2 = 0;
    for (int i = 0; i < N_SAMPLES; i++) {
        mean1 += sig1[i];
        mean2 += sig2[i];
    }
    mean1 /= N_SAMPLES;
    mean2 /= N_SAMPLES;
    
    double std1 = 0, std2 = 0;
    for (int i = 0; i < N_SAMPLES; i++) {
        double d1 = sig1[i] - mean1;
        double d2 = sig2[i] - mean2;
        std1 += d1 * d1;
        std2 += d2 * d2;
    }
    std1 = sqrt(std1 / N_SAMPLES);
    std2 = sqrt(std2 / N_SAMPLES);
    
    if (std1 < 1e-12) std1 = 1;
    if (std2 < 1e-12) std2 = 1;
    
    // 2. Embedding (m=3, tau=5)
    int m = 3, tau = 5;
    int len = N_SAMPLES - (m-1)*tau;
    
    if (len <= 0) {
        for (int i = 0; i < 8; i++) results[i] = 0;
        return;
    }
    
    // Create embedded vectors
    vector<vector<double>> e1(len, vector<double>(m));
    vector<vector<double>> e2(len, vector<double>(m));
    
    for (int i = 0; i < len; i++) {
        for (int j = 0; j < m; j++) {
            e1[i][j] = (sig1[i + j*tau] - mean1) / std1;
            e2[i][j] = (sig2[i + j*tau] - mean2) / std2;
        }
    }
    
    // 3. Build recurrence matrix
    vector<vector<bool>> RM(len, vector<bool>(len, false));
    int rec = 0;
    
    for (int i = 0; i < len; i++) {
        for (int j = 0; j < len; j++) {
            double dist_sq = 0;
            for (int k = 0; k < m; k++) {
                double d = e1[i][k] - e2[j][k];
                dist_sq += d * d;
            }
            if (sqrt(dist_sq) <= R) {
                RM[i][j] = true;
                rec++;
            }
        }
    }
    
    double RR = (double)rec / (len * len);
    
    // 4. Diagonal line analysis
    const int min_diag = 2;
    int d_lines = 0, d_points = 0, d_max = 0;
    double d_avg = 0, d_ent = 0;
    vector<int> d_lengths;
    double d_total = 0;
    
    for (int k = -(len-1); k < len; k++) {
        int cur = 0;
        for (int i = max(0, -k), j = max(0, k); i < len && j < len; i++, j++) {
            if (RM[i][j]) {
                cur++;
            } else {
                if (cur >= min_diag) {
                    d_lines++;
                    d_points += cur;
                    d_lengths.push_back(cur);
                    d_total += cur;
                    if (cur > d_max) d_max = cur;
                }
                cur = 0;
            }
        }
        if (cur >= min_diag) {
            d_lines++;
            d_points += cur;
            d_lengths.push_back(cur);
            d_total += cur;
            if (cur > d_max) d_max = cur;
        }
    }
    
    d_avg = d_lines > 0 ? d_total / d_lines : 0;
    
    // Entropy
    for (int l : d_lengths) {
        double p = (double)l / d_total;
        if (p > 0) d_ent -= p * log2(p);
    }
    
    // 5. Vertical line analysis  
    const int min_vert = 2;
    int v_lines = 0, v_points = 0, v_max = 0;
    double v_avg = 0;
    double v_total = 0;
    
    for (int j = 0; j < len; j++) {
        int cur = 0;
        for (int i = 0; i < len; i++) {
            if (RM[i][j]) {
                cur++;
            } else {
                if (cur >= min_vert) {
                    v_lines++;
                    v_points += cur;
                    v_total += cur;
                    if (cur > v_max) v_max = cur;
                }
                cur = 0;
            }
        }
        if (cur >= min_vert) {
            v_lines++;
            v_points += cur;
            v_total += cur;
            if (cur > v_max) v_max = cur;
        }
    }
    
    v_avg = v_lines > 0 ? v_total / v_lines : 0;
    
    // 6. Final metrics
    double DET = rec > 0 ? (double)d_points / rec : 0;
    double LAM = rec > 0 ? (double)v_points / rec : 0;
    double DIV = d_max > 0 ? 1.0 / d_max : 0;
    
    // 7. Output in QEMU order
    results[0] = DET;      // epsilon
    results[1] = RR;       // recurrence rate
    results[2] = DET;      // determinism
    results[3] = v_avg;    // L (trapping time)
    results[4] = d_max;    // L_max
    results[5] = DIV;      // divergence
    results[6] = d_ent;    // entropy
    results[7] = LAM;      // laminarity
}

static int recv_eventfd(int sock)
{
    struct msghdr msg = {};
    char buf[CMSG_SPACE(sizeof(int))];
    char dummy;
    struct iovec iov = { &dummy, sizeof(dummy) };

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    if (recvmsg(sock, &msg, 0) < 0) {
        perror("[SystemC] recvmsg");
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
        cerr << "[SystemC] No eventfd received" << endl;
        return -1;
    }

    int efd;
    memcpy(&efd, CMSG_DATA(cmsg), sizeof(int));
    cout << "[SystemC] Received eventfd = " << efd << endl;
    return efd;
}


// SystemC module
SC_MODULE(CRQAServer) {
    SC_CTOR(CRQAServer) {
        SC_THREAD(server_thread);
    }
    int eventfd = -1; 

    void server_thread() {
        cout << "[SystemC] Starting CRQA server..." << endl;
        
        // Create socket
        int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (srv_fd < 0) {
            cerr << "[SystemC] socket() failed: " << strerror(errno) << endl;
            return;
        }
        
        // Set socket options
        int opt = 1;
        setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // Remove old socket
        unlink(SOCKET_PATH);
        
        // Bind
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
        
        if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            cerr << "[SystemC] bind() failed: " << strerror(errno) << endl;
            close(srv_fd);
            return;
        }
        
        // Listen
        if (listen(srv_fd, 5) < 0) {
            cerr << "[SystemC] listen() failed: " << strerror(errno) << endl;
            close(srv_fd);
            return;
        }
        
        cout << "[SystemC] Listening on " << SOCKET_PATH << endl;
        cout << "[SystemC] Ready for QEMU connections (keeps connection open)" << endl;
        
        int connection_count = 0;
        
        // Main server loop
        while (true) {
            cout << "\n[SystemC] Waiting for connection..." << endl;
            
            int cli_fd = accept(srv_fd, NULL, NULL);
            if (cli_fd < 0) {
                if (errno == EINTR) continue;
                cerr << "[SystemC] accept() failed: " << strerror(errno) << endl;
                wait(1, SC_SEC);
                continue;
            }
            
            connection_count++;
            cout << "[SystemC] QEMU connected! (fd=" << cli_fd 
                 << ", connection #" << connection_count << ")" << endl;
            cout << "[SystemC] Connection will stay open for multiple requests" << endl;
            
            /* receive eventfd ONCE */
            eventfd = recv_eventfd(cli_fd);
            if (eventfd < 0) {
                cerr << "[SystemC] Failed to receive eventfd" << endl;
                return;
            }

            int request_count = 0;
            bool connection_active = true;
            
            // Handle this connection
            while (connection_active) {
                Input msg;
                
                // BLOCKING READ - waits forever for QEMU
                ssize_t bytes = read(cli_fd, &msg, sizeof(msg));
                
                if (bytes <= 0) {
                    if (bytes == 0) {
                        cout << "[SystemC] QEMU closed the connection" << endl;
                    } else {
                        cerr << "[SystemC] read() error: " << strerror(errno) << endl;
                    }
                    connection_active = false;
                    break;
                }
                
                if (bytes != sizeof(msg)) {
                    cerr << "[SystemC] Incomplete message: " << bytes 
                         << " bytes, expected " << sizeof(msg) << endl;
                    connection_active = false;
                    break;
                }
                
                if (msg.ready) {
                    request_count++;
                    cout << "\n[SystemC] === Processing request #" << request_count << " ===" << endl;
                    cout << "[SystemC] R = " << msg.R << ", opcode = " << msg.opcode << endl;
                    cout << "[SystemC] s1[0] = " << msg.sig1[0] << ", s2[0] = " << msg.sig2[0] << endl;
                    
                    // Compute CRQA
                    Output results;
                    compute_crqa_complete(msg.R, msg.sig1, msg.sig2, (double*)&results);
                    
                    // Send results back
                    ssize_t written = write(cli_fd, &results, sizeof(results));
                    if (written != sizeof(results)) {
                        cerr << "[SystemC] write() error: " << strerror(errno) << endl;
                        connection_active = false;
                        break;
                    }
                    cout << "[SystemC] Results sent to QEMU" << endl;
                    cout << "[SystemC] epsilon=" << results.eps << " RR=" << results.rr 
                         << " DET=" << results.det << " LAM=" << results.lam << endl;
                    cout << "[SystemC] Waiting for next request..." << endl;
		    /*  SIGNAL QEMU */
		    cout << "now writing on QEMU's shared eventfd" << endl;
                    uint64_t one = 1;
                    write(eventfd, &one, sizeof(one)); 


                }
            }
            
            close(cli_fd);
            cout << "[SystemC] Connection #" << connection_count << " closed" << endl;
        }
        
        // Cleanup (never reached in practice)
        close(srv_fd);
        unlink(SOCKET_PATH);
    }
};

// Global for signal handler
CRQAServer* g_server = nullptr;

void signal_handler(int sig) {
    cout << "\n[SystemC] Received signal " << sig << ", shutting down..." << endl;
    sc_stop();
}



// SystemC main function
int sc_main(int argc, char* argv[]) {
    cout << "\n==========================================" << endl;
    cout << "    SystemC CRQA Server - PERSISTENT CONNECTION" << endl;
    cout << "==========================================\n" << endl;
    
    // Setup signal handlers
    //signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create server
    CRQAServer server("server");
    g_server = &server;
    
    cout << "[SystemC] Starting simulation (press Ctrl+C to exit)..." << endl;
    
    // Run forever
    sc_start();
    
    cout << "\n[SystemC] Simulation ended" << endl;
    g_server = nullptr;
    
    return 0;
}
