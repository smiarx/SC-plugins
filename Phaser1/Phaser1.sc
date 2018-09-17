AnalogEcho : UGen {
    *ar { arg in = 0.0, a=[0.1];
        ^this.multiNew(['audio', in]++a);
    }
}
