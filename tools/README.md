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
