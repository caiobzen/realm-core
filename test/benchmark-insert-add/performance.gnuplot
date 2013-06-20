#!/usr/bin/gnuplot
set datafile separator ";"
set terminal pdfcairo color enhanced
set output "performance.pdf"

# ./add_insert -N 50000000 -n 50000 | tee append_table_inmem.dat
set title "In-memory table append"
set xlabel "Number of rows"
set ylabel "Rows/sec"
plot "append_table_inmem.dat"

# ./add_insert -i -N 50000000 -n 50000 | tee insert_table_inmem.dat
set title "In-memory table insert"
set xlabel "Number of rows"
set ylabel "Rows/sec"
plot "insert_table_inmem.dat"

# ./add_insert -s mem -N 50000 -n 500 | tee append_transact_inmem.dat
set title "In-memory transaction append"
set xlabel "Number of rows"
set ylabel "Rows/sec"
plot "append_transact_inmem.dat"

# ./add_insert -s mem -N 50000 -n 500 | tee insert_transact_inmem.dat
set title "In-memory transaction insert"
set xlabel "Number of rows"
set ylabel "Rows/sec"
plot "insert_transact_inmem.dat"

# ./add_insert -s full -N 50000 -n 500 | tee append_transact_full.dat
set title "To-disk transaction append"
set xlabel "Number of rows"
set ylabel "Rows/sec"
plot "append_transact_full.dat"

# ./add_insert -s full -N 50000 -n 500 | tee insert_transact_full.dat
set title "To-disk transaction insert"
set xlabel "Number of rows"
set ylabel "Rows/sec"
plot "insert_transact_full.dat"
