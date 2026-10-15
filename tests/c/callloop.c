// Loops whose body ends in a call of a void function: the loop's jump
// trigger must not coincide with the call trigger.
// expect-exit: 0
static volatile int counter;
static volatile int sink;

__attribute__((noinline)) static void bump(void) { counter++; }
__attribute__((noinline)) static void add(int v) { sink += v; }
__attribute__((noinline)) static int get(int i) { return i * 2; }

int main(void) {
  for (int i = 0; i < 10; i++)
    bump();
  if (counter != 10) return 1;
  for (int i = 0; i < 7; i++)
    add(get(i));
  if (sink != 42) return 2;
  int n = 0;
  while (n < 5) {
    n++;
    bump();
    add(1);
  }
  if (counter != 15 || sink != 47) return 3;
  // Loop-carried values travel in flight around a body ending in a call.
  int acc = 0, odd = 0;
  for (int i = 0; i < 20; i++) {
    acc += i * 3;
    odd += acc & 1;
    add(odd);
  }
  if (acc != 570 || odd != 10 || sink != 47 + 105) return 4;
  return 0;
}
