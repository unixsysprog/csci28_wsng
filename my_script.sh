#/bin/sh -v
#
# My test script to test my submission for wsng.
# It removes any previous compilations, and re-compiles
# the program fresh.
#

#-------------------------------------
#    compile program
#-------------------------------------
make clean
make wsng
