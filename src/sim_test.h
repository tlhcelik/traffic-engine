#ifndef SIM_TEST_H
#define SIM_TEST_H

#include <QStringList>

// sim_test
// --------
// Simulation test without a window (command line, see run()). The test runs
// the simulation as fast as possible with the checks of the simulation
// (overlapping vehicles, vehicles that do not move, deadlocks) and writes a
// report. Use it to find and to verify the corrections of the traffic rules.
//
//   --sim-test <seconds>      simulated time
//   --sim-file <file>         workspace (.te) or OpenStreetMap data (.osm);
//                             without it: the demo workspace
//   --sim-report <file.txt>   report (default: sim_test.txt in the log folder)
//   --sim-seed <n>            seed of the random numbers (other traffic)
//   --sim-stuck <seconds>     a vehicle that does not move for this time is
//                             a problem (default 120)
//   --sim-gridlock <seconds>  gridlock rule for this test (0 = off)
//   --shot-density <percent>  traffic density for this test
//   --sim-clock <hh:mm>       start clock (default: the clock of the workspace)
//   --sim-save <file.te>      saves the loaded workspace (for example the
//                             result of an OpenStreetMap import)

namespace sim_test
{
	// Returns the exit code: 0 no problem, 1 problems found, 2 the test cannot start.
	int run(const QStringList &args);
}

#endif // SIM_TEST_H
