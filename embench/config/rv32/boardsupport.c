/* Embench board support for the rv32imac comparison images on Spike.

   The same arrangement as embench/config/scry: start_trigger/stop_trigger
   are real functions whose first execution scripts/rv32_metrics.py finds
   in Spike's instruction log to delimit the benchmark window.  main()
   returns 0 when verify_benchmark() succeeded, which crt0.S passes to the
   host as Spike's exit code.  */

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
