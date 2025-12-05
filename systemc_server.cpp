// systemc_server.cpp — FINAL WORKING VERSION (Dec 2025)
#include <systemc>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <cstring>

using namespace std;
using namespace sc_core;

#define SOCKET_PATH "/tmp/crqa_socket"
#define N_SAMPLES 512

SC_MODULE(CRQAModule) {
    sc_in<double>  in_R;
    sc_in<double>  in_sig1[N_SAMPLES];
    sc_in<double>  in_sig2[N_SAMPLES];
    sc_in<bool>    in_valid;

    sc_out<double> out_epsilon;
    sc_out<double> out_recurrence_rate;
    sc_out<double> out_determinism;
    sc_out<double> out_laminarity;
    sc_out<double> out_trapping_time;
    sc_out<double> out_max_diag_line;
    sc_out<double> out_divergence;
    sc_out<double> out_entropy;
    sc_out<bool>   out_complete;

    const int m = 3;
    const int tau = 5;
    const int min_diag = 2;
    const int min_vert = 2;

    SC_CTOR(CRQAModule) {
        SC_METHOD(compute_crqa);
        sensitive << in_valid.pos();
        dont_initialize();           // THIS FIXES E112 FOREVER
    }

    void compute_crqa();

private:
    vector<vector<double>> embed(const vector<double>& s) {
        int len = N_SAMPLES - (m-1)*tau;
        if (len <= 0) return {};
        vector<vector<double>> e(len, vector<double>(m));
        for (int i = 0; i < len; ++i)
            for (int j = 0; j < m; ++j)
                e[i][j] = s[i + j*tau];
        return e;
    }

    double dist(const vector<double>& a, const vector<double>& b) {
        double sum = 0.0;
        for (int i = 0; i < m; ++i) {
            double d = a[i] - b[i];
            sum += d*d;
        }
        return sqrt(sum);
    }

    vector<double> normalize(const vector<double>& s) {
        double sum = 0.0, sum2 = 0.0;
        for (double v : s) sum += v;
        double mean = sum / N_SAMPLES;
        for (double v : s) sum2 += (v - mean)*(v - mean);
        double std = sqrt(sum2 / N_SAMPLES);
        if (std < 1e-12) return s;
        vector<double> n(N_SAMPLES);
        for (int i = 0; i < N_SAMPLES; ++i)
            n[i] = (s[i] - mean) / std;
        return n;
    }

    void analyze_diag(const vector<vector<bool>>& R,
                      int& lines, int& points, double& avg, int& maxl, double& ent) {
        lines = points = maxl = 0;
        avg = ent = 0.0;
        int N = R.size();
        vector<int> lengths;
        double total = 0.0;

        for (int k = -(N-1); k < N; ++k) {
            int cur = 0;
            for (int i = max(0, -k), j = max(0, k); i < N && j < N; ++i, ++j) {
                if (R[i][j]) ++cur;
                else {
                    if (cur >= min_diag) {
                        ++lines; points += cur; lengths.push_back(cur); total += cur;
                        maxl = max(maxl, cur);
                    }
                    cur = 0;
                }
            }
            if (cur >= min_diag) {
                ++lines; points += cur; lengths.push_back(cur); total += cur;
                maxl = max(maxl, cur);
            }
        }
        avg = lines ? total / lines : 0.0;
        for (int l : lengths) {
            double p = l / total;
            if (p > 0) ent -= p * log2(p);
        }
    }

    void analyze_vert(const vector<vector<bool>>& R,
                      int& lines, int& points, double& avg, int& maxl) {
        lines = points = maxl = 0;
        avg = 0.0;
        int N = R.size();
        double total = 0.0;

        for (int j = 0; j < N; ++j) {
            int cur = 0;
            for (int i = 0; i < N; ++i) {
                if (R[i][j]) ++cur;
                else {
                    if (cur >= min_vert) {
                        ++lines; points += cur; total += cur;
                        maxl = max(maxl, cur);
                    }
                    cur = 0;
                }
            }
            if (cur >= min_vert) {
                ++lines; points += cur; total += cur;
                maxl = max(maxl, cur);
            }
        }
        avg = lines ? total / lines : 0.0;
    }
};

void CRQAModule::compute_crqa() {
    if (!in_valid.read()) return;   // only on rising edge
    out_complete.write(false); 
    double R = in_R.read();
    vector<double> s1(N_SAMPLES), s2(N_SAMPLES);
    for (int i = 0; i < N_SAMPLES; ++i) {
        s1[i] = in_sig1[i].read();
        s2[i] = in_sig2[i].read();
    }

    auto n1 = normalize(s1);
    auto n2 = normalize(s2);
    auto e1 = embed(n1);
    auto e2 = embed(n2);

    // If embedding failed → output zeros
    if (e1.empty() || e2.empty()) {
        out_epsilon.write(0);
        out_recurrence_rate.write(0);
        out_determinism.write(0);
        out_laminarity.write(0);
        out_trapping_time.write(0);
        out_max_diag_line.write(0);
        out_divergence.write(0);
        out_entropy.write(0);
        out_complete.write(true);
        return;
    }

    int N = e1.size();
    vector<vector<bool>> RM(N, vector<bool>(N, false));
    int rec = 0;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            if (dist(e1[i], e2[j]) <= R) {
                RM[i][j] = true;
                ++rec;
            }

    double RR = double(rec) / (N * N);
    out_recurrence_rate.write(RR);

    // Diagonal lines
    int d_lines = 0, d_points = 0, d_max = 0;
    double d_avg = 0.0, d_ent = 0.0;
    analyze_diag(RM, d_lines, d_points, d_avg, d_max, d_ent);

    // Vertical lines
    int v_lines = 0, v_points = 0, v_max = 0;
    double v_avg = 0.0;
    analyze_vert(RM, v_lines, v_points, v_avg, v_max);

    double DET = rec ? double(d_points) / rec : 0.0;
    double LAM = rec ? double(v_points) / rec : 0.0;

    out_determinism.write(DET);
    out_laminarity.write(LAM);
    out_trapping_time.write(v_avg);
    out_max_diag_line.write(d_max);
    out_divergence.write(d_max ? 1.0 / d_max : 0.0);
    out_entropy.write(d_ent);
    out_epsilon.write(DET);
    out_complete.write(true);

    cout << "[CRQA] Done → RR=" << RR << " DET=" << DET << " LAM=" << LAM << endl;
}

SC_MODULE(ServerTop) {
    sc_signal<double> r_sig;
    sc_signal<double> s1_sig[N_SAMPLES];
    sc_signal<double> s2_sig[N_SAMPLES];
    sc_signal<bool>   valid_sig;
    sc_signal<double> eps_sig, rr_sig, det_sig, lam_sig, tt_sig, maxd_sig, div_sig, ent_sig;
    sc_signal<bool>   complete_sig;

    CRQAModule crqa{"crqa"};

    int srv_fd = -1, cli_fd = -1;

    SC_CTOR(ServerTop) {
        crqa.in_R(r_sig);
        crqa.in_valid(valid_sig);
        for (int i = 0; i < N_SAMPLES; ++i) {
            crqa.in_sig1[i](s1_sig[i]);
            crqa.in_sig2[i](s2_sig[i]);
        }
        crqa.out_epsilon(eps_sig);
        crqa.out_recurrence_rate(rr_sig);
        crqa.out_determinism(det_sig);
        crqa.out_laminarity(lam_sig);
        crqa.out_trapping_time(tt_sig);
        crqa.out_max_diag_line(maxd_sig);
        crqa.out_divergence(div_sig);
        crqa.out_entropy(ent_sig);
        crqa.out_complete(complete_sig);

        SC_THREAD(server_thread);
    }

    void server_thread() {
        srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        unlink(SOCKET_PATH);
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, SOCKET_PATH);
        bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr));
        listen(srv_fd, 5);
        cout << "[SystemC] Listening on " << SOCKET_PATH << endl;

        struct Input  { double R; double s1[N_SAMPLES]; double s2[N_SAMPLES]; bool ready; };
        struct Output { double eps, rr, det, lam, tt, maxd, div, ent; };

        while (true) {
            cout << "[SystemC] Waiting for connection...\n";
            cli_fd = accept(srv_fd, nullptr, nullptr);
            if (cli_fd < 0) { wait(1, SC_SEC); continue; }
            cout << "[SystemC] QEMU connected!\n";

            while (true) {
                struct pollfd pfd{cli_fd, POLLIN, 0};
                if (poll(&pfd, 1, 1000) <= 0) continue;

                Input msg;
                ssize_t n = read(cli_fd, &msg, sizeof(msg));
                if (n != sizeof(msg)) break;

                if (msg.ready) {
                    r_sig.write(msg.R);
                    for (int i = 0; i < N_SAMPLES; ++i) {
                        s1_sig[i].write(msg.s1[i]);
                        s2_sig[i].write(msg.s2[i]);
                    }
                    wait(SC_ZERO_TIME);
                    valid_sig.write(true);
                    wait(SC_ZERO_TIME);
                    valid_sig.write(false);

                    while (!complete_sig.read())
                        wait(10, SC_MS);

                    Output resp{
                        eps_sig.read(), rr_sig.read(), det_sig.read(), lam_sig.read(),
                        tt_sig.read(), maxd_sig.read(), div_sig.read(), ent_sig.read()
                    };
                    write(cli_fd, &resp, sizeof(resp));
                    cout << "[SystemC] Sent results\n";
                }
                //complete_sig.write(false);
            }
            close(cli_fd); cli_fd = -1;
        }
    }
};

int sc_main(int argc, char* argv[]) {
    ServerTop top("top");
    cout << "\n=== SystemC CRQA Server READY ===\n";
    sc_start();
    return 0;
}
