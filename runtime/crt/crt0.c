/* crt0 for Scry: the ELF entry point.
 *
 * The scryer simulator starts executing at the ELF entry address with an
 * empty call stack.  When the entry function returns, the simulation ends and
 * the values delivered through the return trigger become the "returned
 * operands"; in --machine-mode the low byte of the first one is the process
 * exit code.  So _start simply calls main() and returns its result.
 *
 * .bss needs no clearing: the ELF loader zero-fills memsz > filesz.
 */

extern int main(int argc, char **argv);
extern void __libc_init(void);

int _start(void) {
  __libc_init();
  return main(0, (char **)0);
}
