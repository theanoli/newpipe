import os
import re
import sys
import math
import pandas as pd
import numpy as np
import copy
import pickle
import subprocess

import plotly
import plotly.plotly as py
from plotly.offline import download_plotlyjs, init_notebook_mode, iplot
from plotly.graph_objs import *
#init_notebook_mode()

home = os.getcwd()
with open(os.path.join(home, 'experiment_dict.pickle'), 'rb') as f:
    experiment_dict = pickle.load(f)

data = []
for exp, exp_data in experiment_dict.items():
    x = exp_data['throughput']['timestamp']
    y = exp_data['throughput']['tput']
    text = []

    data.append(Scatter(x=x,
                        y=y,
                        name="%d clients" % exp_data['nclients'],
                        mode='lines+markers',))

    layout = Layout(
            title='Throughput over time for single client machine, multiple client thread echo server',
            showlegend=True,
            xaxis=dict(title='time (s)'),
            yaxis=dict(title='throughput (pps)')
    )

    fig = Figure(data=data, layout=layout)
    plotly.offline.plot(fig)

