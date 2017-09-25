#!/usr/bin/env python3
# Copyright 2017 H2O.ai; Apache License Version 2.0;  -*- encoding: utf-8 -*-
import datatable as dt


def test_issue365():
    """
    This is to verify that all data written to CSV can be safely read back,
    and that no rows were accidentally omitted.
    """
    # 999983 is prime; we want to have the number of rows not evenly divisible
    # by the number of threads.
    d0 = dt.DataTable({"A": list(range(999983)), "B": [5] * 999983})
    d1 = dt.fread(text=d0.to_csv())
    assert d1.shape == d0.shape
    assert d1.names == d0.names
    assert d1.types == d0.types
    assert d1.topython() == d0.topython()


def test_save_simple():
    d = dt.DataTable([[1, 4, 5], [True, False, None], ["foo", None, "bar"]],
                     colnames=["A", "B", "C"])
    out = d.to_csv()
    assert out == "A,B,C\n1,1,foo\n4,0,\n5,,bar\n"