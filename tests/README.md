## Acceleration Test for Correctness

Run `make` or `acc_test.sh` to run a simulation with 30K subsampled particles from the *lambs* benchmark in ChaNGa.
This test will compare the particle accelerations with the known baseline in `direct.acc` and output the relative force errors.
`make clean` will remove the intermediate and final output files generated by the testing harness.
