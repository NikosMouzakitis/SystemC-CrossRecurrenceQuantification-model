#include <systemc>
#include <iostream>
#include <fstream>
#include "systemc_psd_epsilon.h"

using namespace std;
using namespace sc_core;

// Load 512 samples from a text file
bool load_signal(const string &path, double *buffer)
{
    ifstream fin(path);
    if (!fin.is_open()) {
        cerr << "ERROR: Cannot open file: " << path << endl;
        return false;
    }

    for (int i = 0; i < 512; i++) {
        if (!(fin >> buffer[i])) {
            cerr << "ERROR: File " << path << " has less than 512 samples.\n";
            return false;
        }
    }

    return true;
}

int sc_main(int argc, char *argv[])
{
    double sig1[512];
    double sig2[512];

    if (!load_signal("systemc_input_F7_T7.txt", sig1)) return -1;
    if (!load_signal("systemc_input_FP1_F7.txt", sig2)) return -1;

    // FIFOs
    sc_fifo<double> r_fifo(1);
    sc_fifo<double> s1_fifo[512];
    sc_fifo<double> s2_fifo[512];
    sc_fifo<double> eps_fifo(1);

    // Module
    PSDEpsilonModule module("PSDEpsilonModule");

    // Binding ports
    module.in_R(r_fifo);
    for (int i = 0; i < 512; i++)
        module.in_sig1[i](s1_fifo[i]);

    for (int i = 0; i < 512; i++)
        module.in_sig2[i](s2_fifo[i]);

    module.out_epsilon(eps_fifo);

    // Push input R = 0.15
    r_fifo.write(0.15);

    // Feed both signals
    for (int i = 0; i < 512; i++)
        s1_fifo[i].write(sig1[i]);

    for (int i = 0; i < 512; i++)
        s2_fifo[i].write(sig2[i]);

    // Start simulation
    sc_start(1, SC_MS);

    // Read output
    double epsilon = eps_fifo.read();
    cout << "FINAL EPSILON = " << epsilon << endl;

    return 0;
}

