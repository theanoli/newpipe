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
results = os.path.join(home, 'results')
with open(os.path.join(results, 'experiment_dict.pickle'), 'rb') as f:
    experiment_dict = pickle.load(f)

data_dict = {}

def throughput():
    print("Generating throughput over time...")
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
        plotly.offline.plot(fig, 
                filename=os.path.join(results, 'throughput.html'))


def latency_hist():
    print("Generating latency histogram...")
    data = []
    for exp, exp_data in experiment_dict.items():
        latencies = pd.concat([x for x in exp_data['latency']], ignore_index=True)
        x = latencies['latency']

        data.append(Histogram(x=x,
                            name="%d clients" % exp_data['nclients'],
                            opacity=0.6))

        layout = Layout(
                title='Latency over time for single client machine, multiple client thread echo server',
                showlegend=True,
                xaxis=dict(title='sendtime'),
                yaxis=dict(title='latency (usec)')
        )

        fig = Figure(data=data, layout=layout)
        plotly.offline.plot(fig, 
                filename=os.path.join(results, 'latency_hist.html'))


def latency_box():
    print("Generating latency boxplot...")
    data = []
    data_dict = {}

    for exp, exp_data in experiment_dict.items():
        latencies = pd.concat([x for x in exp_data['latency']], ignore_index=True)
        data_dict[exp_data['nclients']] = latencies['latency']

    for nclients in sorted(data_dict.keys()):
        y = data_dict[nclients]
        data.append(Box(y=y,
                            name="%d clients" % nclients,
                            ))

        layout = Layout(
                title='Latency boxplot, multiple client thread echo server',
                showlegend=False,
                xaxis=dict(title='nclients'),
                yaxis=dict(title='latency (usec)')
        )

        fig = Figure(data=data, layout=layout)
        plotly.offline.plot(fig, 
                filename=os.path.join(results, 'latency_box.html'))


# Throughput-latency
def throughput_latency():
    print("Generating throughput-latency...")
    for exp, exp_data in experiment_dict.items():
        nclients = exp_data['nclients']
        data_dict[nclients] = {}
        latencies = pd.concat([x for x in exp_data['latency']], ignore_index=True)
        data_dict[nclients]['latency'] = latencies['latency'].median()
        data_dict[nclients]['tput'] = exp_data['throughput']['tput'].mean()

    keys = sorted(data_dict.keys())
    x = [data_dict[key]['tput'] for key in keys]  # throughout
    y = [data_dict[key]['latency'] for key in keys] # latency

    data = Scatter(x=x,
            y=y,
            mode='lines+markers+text',
            textposition='top left',
            text=["%d clients" % key for key in keys],
            )

    layout = Layout(
            title="Mean throughput/median latency for single client machine, multiple client thread echo server",
            xaxis=dict(title='throughput (pps)'),
            yaxis=dict(title='latency (usec)'),
            )

    fig = Figure(data=[data], layout=layout)
    plotly.offline.plot(fig, 
            filename=os.path.join(results, 'tputlatency.html'))


print("--------------------------------------------------------------------------------")
print("Generating plots!")
throughput_latency()
throughput()
latency_hist()
latency_box()
print("--------------------------------------------------------------------------------\n")
