// The low byte of main's return value is the simulator exit code.
// expect-exit: 42
volatile int v = 40;

int main(void) { return v + 2; }
