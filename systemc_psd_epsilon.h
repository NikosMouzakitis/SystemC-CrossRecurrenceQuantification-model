#ifndef SYSTEMC_PSD_EPSILON_H
#define SYSTEMC_PSD_EPSILON_H

#include <systemc>
#include <cmath>
#include <array>

using namespace sc_core;

// -----------------------------------------------------------------------------
// SystemC module computing epsilon = R * mean(psd1, psd2)
// -----------------------------------------------------------------------------
SC_MODULE(PSDEpsilonModule)
{
    // Inputs
    sc_fifo_in<double> in_R;             // R factor (e.g. 0.15)
    sc_fifo_in<double> in_sig1[512];     // 512 samples of signal 1
    sc_fifo_in<double> in_sig2[512];     // 512 samples of signal 2

    // Output
    sc_fifo_out<double> out_epsilon;

    static const int WINDOW_SIZE = 512;
    static const int m = 3;
    static const int tau = 1;
    static const int N = WINDOW_SIZE - (m - 1) * tau; // = 510

    SC_CTOR(PSDEpsilonModule)
    {
        SC_THREAD(process);
    }

    // Helper: embed into 3D vectors
    void embed_3d(const double *x, std::array<std::array<double,3>,N> &emb)
    {
        for (int i = 0; i < N; i++) {
            emb[i][0] = x[i];
            emb[i][1] = x[i + tau];
            emb[i][2] = x[i + 2 * tau];
        }
    }

    // Helper: compute PSD (max 3D distance)
    double compute_psd(const std::array<std::array<double,3>,N> &emb)
    {
        double maxd = 0.0;

        for (int i = 0; i < N; i++) {
            for (int j = i+1; j < N; j++) {
                double dx = emb[i][0] - emb[j][0];
                double dy = emb[i][1] - emb[j][1];
                double dz = emb[i][2] - emb[j][2];
                double d2 = dx*dx + dy*dy + dz*dz;
                if (d2 > maxd) maxd = d2;
            }
        }
        return std::sqrt(maxd);
    }

    // Main SystemC process
    void process()
    {
        while (true)
        {
            // ---- Read R ----
            double R = in_R.read();

            // ---- Read inputs (512 samples each) ----
            double sig1[WINDOW_SIZE];
            double sig2[WINDOW_SIZE];

            for (int i = 0; i < WINDOW_SIZE; i++)
                sig1[i] = in_sig1[i].read();

            for (int i = 0; i < WINDOW_SIZE; i++)
                sig2[i] = in_sig2[i].read();

            // ---- Embed both signals ----
            std::array<std::array<double,3>,N> emb1;
            std::array<std::array<double,3>,N> emb2;

            embed_3d(sig1, emb1);
            embed_3d(sig2, emb2);

            // ---- Compute PSD ----
            double psd1 = compute_psd(emb1);
            double psd2 = compute_psd(emb2);

            // ---- Compute epsilon ----
            double epsilon = R * (psd1 + psd2) / 2.0;

            // ---- Output ----
            out_epsilon.write(epsilon);
        }
    }
};

#endif

