TOOLS
=====

playdiff
--------

Computes metrics of the deviations between two `loadplay(1)` generated
outputs.

```
usage: tools/playdiff file1 file2 ...
```

The output of `loadplay(1)` is not reproducible. Due to differences
in timing between each run there are slight variations in the load
that a powerd samples. This makes it difficult to tell whether a second
run with a different parameter set or a different powerd version
exhibits different behaviour, which is important for regression testing.

The most intuitive way of dealing with this is plotting a graph. The
`playdiff` tool instead provides metrics to make the same judgement.

### Metrics

The `playdiff` tool integrates the deviations and absolute deviations
between two `loadplay` outputs over time. These values are used to
present four metrics per column of `loadplay` output:

- Integral over Deviations (ID)
- Mean Deviation (MD)
- Integral over Absolute Deviations (IAD)
- Mean Absolute Deviation (MAD)

### Interpreting the Data

The integrals and means provide the same information, but the magnitude
of the means is independent of the duration of the load replay, thus
the means make it easier to interpret the data.

The following excerpt of a real dataset, shows the IAD looks high,
the MAD is a much better presentation. An average CPU frequency deviation
of 34 MHz is noteworthy, but not indicative of a fundamental difference.

A look at the MAD column of the `run.load` row shows that `loadplay`
presented different load data to the powerd between runs. The `rec.load`
row confirms that both runs are based on the same recording. However
the ID column shows that the accumulated deviation over the entire run
is less than 0.05 MHz. This is indicative of an aliasing effect that
implies there was a small time offset between both runs, apart from
that performance of the powerd was the same.

```
--- a/load.play
+++ b/load.play
                                ID            MD           IAD           MAD
time[s]                        0.0           0.0           0.0           0.0
cpu.0.rec.freq[MHz]            0.0           0.0           0.0           0.0
cpu.0.rec.load[MHz]            0.0           0.0           0.0           0.0
cpu.0.run.freq[MHz]          -94.0          -3.1        1016.0          33.9
cpu.0.run.load[MHz]            0.0           0.0         160.0           5.3
```

playfilter
----------

Post-process loadplay(1) output.

```
usage: tools/playfilter [ filters... ] [--] [ files... ]
```

Takes an optional list of filters and an optional list of files. The
first argument not matching the syntax for a filter is treated as
a file. Alternatively the `--` argument can be provided to mark the
end of the list of filters. This allows providing file names that
look like filters.

The syntax for a filter is `FILTER=ARG[,...]`. Individual filters
are described in the [Filters](#filters) subsection.

### Files

If no file names are given, `stdin` is used as the input. Otherwise
the given files are concatenated.
Each line of input is expected to contain a fixed number of fields
separated by white space. The first line of each file is referred
to as the header and expected to contain the column names.

Subsequent headers are discarded if they match the first file's header.
A mismatch is treated as an error.

### Filters

The following filters are supported.

| Filter    | Arguments       | Describe                                    |
|-----------|-----------------|---------------------------------------------|
| cut       | glob            | Remove unmatched columns                    |
| movingavg | glob pre [post] | Apply a moving average (mean)               |
| subsample | n               | Only output every nth sample                |
| patch     | glob            | Patch concatenated x column                 |
| clone     | glob n          | Clone matched columns n times               |
| hmax      | glob            | Add column with the max of matched columns  |
| hmin      | glob            | Add column with the min of matched columns  |
| hsum      | glob            | Add column with the sum of matched columns  |
| havg      | glob            | Add column with the mean of matched columns |
| precision | glob digits     | Set a fixed amount of fraction digits       |
| style     | format          | Format output (must be the last filter)     |

#### Selecting Columns

The `glob` argument of a filter is used to select the columns to apply
a filter to. The pattern should match the names of the columns without
the unit, an optional square bracket enclosure at the end of a column
name.

Note the that the horizontal filters `hmax`, `hmin`, `hsum` and `havg`
require that all matched columns have the same unit.

#### Pretty Printing

The following filters can be used to customise output:

- `cut=GLOB`
- `precision=GLOB,DIGITS`
- `style=FORMAT`

The `cut` filter selects a subset of columns to output:

```
# obj/loadplay -i loads/freq_tracking.load -o replay.csv obj/powerd++
# tools/playfilter cut='time|cpu.3.*' -- replay.csv
time[s] cpu.3.rec.freq[MHz] cpu.3.rec.load[MHz] cpu.3.run.freq[MHz] cpu.3.run.load[MHz]
0.025 1700 850.0 1700 850.0
0.050 1700 0.0 1700 0.0
0.075 1700 566.7 1700 566.7
0.100 1700 0.0 1700 0.0
...
```

The `precision` filter sets a fixed number of fraction digits for
the matched columns:

```
# tools/playfilter cut='time|cpu.3.*' precision='*.load',3 -- replay.csv
time[s] cpu.3.rec.freq[MHz] cpu.3.rec.load[MHz] cpu.3.run.freq[MHz] cpu.3.run.load[MHz]
0.025 1700 850.000 1700 850.000
0.050 1700 0.000 1700 0.000
0.075 1700 566.700 1700 566.700
0.100 1700 0.000 1700 0.000
...
```

The `style` filter is only allowed as the last filter in the pipeline,
because it produces output that is not valid filter input. It formats
the output for different applications, the supported styles are:

- `CSV`: Fields are separated by a `,` and column names are quoted using `"`
- `MD`:  The output is formatted as a markdown table

```
# tools/playfilter cut='time|cpu.3.*' precision='*.load',3 style=md -- replay.csv
| time[s] | cpu.3.rec.freq[MHz] | cpu.3.rec.load[MHz] | cpu.3.run.freq[MHz] | cpu.3.run.load[MHz] |
|--------:|--------------------:|--------------------:|--------------------:|--------------------:|
|   0.025 |                1700 |             850.000 |                1700 |             850.000 |
|   0.050 |                1700 |               0.000 |                1700 |               0.000 |
|   0.075 |                1700 |             566.700 |                1700 |             566.700 |
|   0.100 |                1700 |               0.000 |                1700 |               0.000 |
...
```

#### Subsampling

The following filters can be used for subsampling:

- `subsample=N`
- `movingavg=GLOB,PRE[,POST]`

If only a subset of the available lines is required, the `subsample`
filter can be used:

```
# tools/playfilter cut='time|cpu.3.*' subsample=4 precision='*.load',3 style=md -- replay.csv
| time[s] | cpu.3.rec.freq[MHz] | cpu.3.rec.load[MHz] | cpu.3.run.freq[MHz] | cpu.3.run.load[MHz] |
|--------:|--------------------:|--------------------:|--------------------:|--------------------:|
|   0.100 |                1700 |               0.000 |                1700 |               0.000 |
|   0.200 |                1700 |               0.000 |                1700 |               0.000 |
|   0.300 |                1700 |               0.000 |                1700 |               0.000 |
|   0.400 |                1700 |               0.000 |                1700 |               0.000 |
...
```

The above example uses every fourth sample, however that means the
information of the other 3 samples is not used. This can be avoided
by applying a low-pass filter:

```
# tools/playfilter cut='time|cpu.3.*' movingavg='cpu*',4 subsample=4 precision='*.load',3 style=md -- replay.csv
| time[s] | cpu.3.rec.freq[MHz] | cpu.3.rec.load[MHz] | cpu.3.run.freq[MHz] | cpu.3.run.load[MHz] |
|--------:|--------------------:|--------------------:|--------------------:|--------------------:|
|   0.100 |                1700 |             354.175 |                1700 |             354.175 |
|   0.200 |                1700 |               0.000 |                1700 |               0.000 |
|   0.300 |                1700 |               0.000 |                1700 |               0.000 |
|   0.400 |                1700 |               0.000 |                1700 |               0.000 |
...
```

The above example uses a four sample pre-filter, i.e. every sample
contains the mean value of the last four samples. Synchronised to
the subsampling interval this results in the reported sample containing
the mean of the original samples without overlap. For this example
the `0.100 s` sample contains the mean of the original `0.025 s`,
`0.050 s`, `0.075 s` and `0.100 s` samples.

#### Imitating `powerd(8)` Sampling

The default sample time of `powerd(8)` is `0.250 s`:

```
# tools/playfilter cut='time|cpu.0.*.load' movingavg='cpu.*',10 subsample=10 precision='*.load',3 style=md -- replay.csv
| time[s] | cpu.0.rec.load[MHz] | cpu.0.run.load[MHz] |
|--------:|--------------------:|--------------------:|
|   0.250 |             396.670 |             396.670 |
|   0.500 |             170.000 |             170.000 |
|   0.750 |               0.000 |               0.000 |
|   1.000 |             405.000 |             405.000 |
...
```

However `powerd(8)` uses the sum of the load of all cores. This can
be achieved using one of the horizontal family of filters:

- `hmax=GLOB` (horizontal maximum)
- `hmin=GLOB` (horizontal minimum)
- `hsum=GLOB` (horizontal sum)
- `havg=GLOB` (horizontal mean)

This set of filters creates a new column by aggregating data from
the matched columns:

```
# tools/playfilter movingavg='cpu.*',10 subsample=10 hsum='*.run.load' hsum='*.rec.load' cut='time|sum*' precision='sum*',3 style=md -- replay.csv
| time[s] | sum(cpu.{0,1,2,3}.run.load)[MHz] | sum(cpu.{0,1,2,3}.rec.load)[MHz] |
|--------:|---------------------------------:|---------------------------------:|
|   0.250 |                         1048.340 |                         1048.340 |
|   0.500 |                          212.500 |                          212.500 |
|   0.750 |                            0.000 |                            0.000 |
|   1.000 |                         2115.000 |                         2115.000 |
...
```

Note there are separate filter steps for the `run.load` and `rec.load`
columns to create two separate sums.

#### Imitating `powerd++(8)` Sampling and Filtering

The default sample rate of `powerd++(8)` is `0.5 s` and instead of
the sum it uses the maximum. On top of it, it uses the mean of the
last 4 sampled maxima:

```
# tools/playfilter movingavg='cpu.*',20 subsample=20 hmax='*.run.load' hmax='*.rec.load' movingavg='max*',4 cut='time|max*' precision='max*',3 style=md -- replay.csv
| time[s] | max(cpu.{0,1,2,3}.run.load)[MHz] | max(cpu.{0,1,2,3}.rec.load)[MHz] |
|--------:|---------------------------------:|---------------------------------:|
|   0.500 |                          283.335 |                          283.335 |
|   1.000 |                          294.168 |                          294.168 |
|   1.500 |                          446.112 |                          449.445 |
|   2.000 |                          525.521 |                          526.771 |
...
```

#### Side by Side Filter Comparisons

Columns can be reproduced, so different filters can be applied to
the same data:

- `clone=GLOB,N`

This can be used to compare the effects of different filters:

```
# tools/playfilter cut='time|cpu.0.rec.load' clone='*.load',2 movingavg='*.load.0',80 movingavg='*.load.1',40,40 precision='cpu.*',3 style=md -- replay.csv
| time[s] | cpu.0.rec.load[MHz] | cpu.0.rec.load.0[MHz] | cpu.0.rec.load.1[MHz] |
|--------:|--------------------:|----------------------:|----------------------:|
|   0.025 |            1700.000 |              1700.000 |               236.993 |
|   0.050 |            1700.000 |              1700.000 |               259.921 |
|   0.075 |             566.700 |              1322.230 |               281.784 |
|   0.100 |               0.000 |               991.675 |               302.652 |
...
```

The column `cpu.0.rec.load` contains the original data, `cpu.0.rec.load.0`
applies a `2 s` moving average. The `cpu.0.rec.load.1` column contains
a symmetric `2 s` moving average (i.e. `1 s` pre and `1 s` post),
which is the best in hindsight representation of a filtered value.

Plotting these illustrates that this produces the same curve with a
`1 s` offset. This illustrates how a `2 s` moving average causes `1 s`
of latency reacting to load events like spikes and drops.

#### Serialising Multiple Replays

It is possible to concatenate multiple replays, but it usually requires
patching the `time` column:

- `patch=GLOB`

Without patching, the time column jumps back down when transitioning
from one file to the next:

```
# tools/playfilter movingavg='*.run.load',20 subsample=20 hmax='*.run.load' cut='time|max*|cpu.0.run.freq' movingavg='max*',4 precision=time,3 precision='max*',1 style=md -- replay.csv replay.csv
| time[s] | cpu.0.run.freq[MHz] | max(cpu.{0,1,2,3}.run.load)[MHz] |
|--------:|--------------------:|---------------------------------:|
|   0.500 |                1700 |                            283.3 |
|   1.000 |                1400 |                            294.2 |
|   1.500 |                1200 |                            446.1 |
|   2.000 |                1300 |                            525.5 |
...
|  28.500 |                1800 |                            732.8 |
|  29.000 |                2000 |                            665.3 |
|  29.500 |                1900 |                            690.1 |
|  30.000 |                1900 |                            810.0 |
|   0.500 |                1700 |                            593.3 |
|   1.000 |                1400 |                            650.8 |
|   1.500 |                1200 |                            525.8 |
|   2.000 |                1300 |                            525.5 |
...
|  28.500 |                1800 |                            732.8 |
|  29.000 |                2000 |                            665.3 |
|  29.500 |                1900 |                            690.1 |
|  30.000 |                1900 |                            810.0 |
```

The `patch` filter uses the previous value as an offset for following values
if the new value is less than or equal to the previous one:

```
# tools/playfilter patch=time movingavg='*.run.load',20 subsample=20 hmax='*.run.load' cut='time|max*|cpu.0.run.freq' movingavg='max*',4 precision=time,3 precision='max*',1 style=md -- replay.csv replay.csv
| time[s] | cpu.0.run.freq[MHz] | max(cpu.{0,1,2,3}.run.load)[MHz] |
|--------:|--------------------:|---------------------------------:|
|   0.500 |                1700 |                            283.3 |
|   1.000 |                1400 |                            294.2 |
|   1.500 |                1200 |                            446.1 |
|   2.000 |                1300 |                            525.5 |
...
|  28.500 |                1800 |                            732.8 |
|  29.000 |                2000 |                            665.3 |
|  29.500 |                1900 |                            690.1 |
|  30.000 |                1900 |                            810.0 |
|  30.500 |                1700 |                            593.3 |
|  31.000 |                1400 |                            650.8 |
|  31.500 |                1200 |                            525.8 |
|  32.000 |                1300 |                            525.5 |
...
|  58.500 |                1800 |                            732.8 |
|  59.000 |                2000 |                            665.3 |
|  59.500 |                1900 |                            690.1 |
|  60.000 |                1900 |                            810.0 |
```
