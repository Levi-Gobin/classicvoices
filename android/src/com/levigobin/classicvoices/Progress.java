package com.levigobin.classicvoices;

/** How a long import reports what it is doing, and learns it should stop. */
interface Progress {
    void step(String message);

    /** 0..1, or a negative value when the amount of work is unknown. */
    void fraction(double f);

    boolean cancelled();

    final class Cancelled extends java.io.IOException {
        Cancelled() {
            super("cancelled");
        }
    }
}
