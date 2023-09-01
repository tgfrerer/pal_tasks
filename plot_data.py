#!/usr/bin/python

import sys
import pandas as pd
import matplotlib.pyplot as plt
import ipdb

if len(sys.argv) < 2:
    print("missing report filename.")

REPORT_FILENAME = sys.argv[1]

test_data = pd.read_csv(REPORT_FILENAME, engine="python", delimiter="\\t", skiprows=[0,1])
test_data= test_data.sort_values(by='Thread count')
test_data= test_data.set_index('Thread count')

test_data.plot(kind='bar', title=REPORT_FILENAME).get_figure().savefig("{}.png".format(REPORT_FILENAME.split(".")[0]))

print(test_data)
# ipdb.set_trace()
