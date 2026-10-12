/* Embench board support for the Scry ISA on the scryer simulator.

   The simulator has no timers and no I/O.  start_trigger/stop_trigger store
   marker values that are visible in the simulator's final state; the
   instruction count is read from the simulator metrics after the run.  main()
   returns 0 when verify_benchmark() succeeded, which becomes the simulator
   exit code in --machine-mode.  */

#include <support.h>

volatile int __embench_trigger_state;

void
initialise_board (void)
{
  __embench_trigger_state = 0;
}

void __attribute__ ((noinline, used))
start_trigger (void)
{
  __embench_trigger_state = 1;
}

void __attribute__ ((noinline, used))
stop_trigger (void)
{
  __embench_trigger_state = 2;
}
