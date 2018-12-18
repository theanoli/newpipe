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

title = "with increasing server thread count,<br>16 client machines, 32 threads each, 1 trial"

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
                            name="%ds, %dcli" % (exp_data['nserver_threads'], exp_data['nclients']),
                            mode='lines+markers',))

        layout = Layout(
                title='Throughput over time per trial %s' % title,
                showlegend=True,
                xaxis=dict(title='time (s)'),
                yaxis=dict(title='throughput (pps)')
        )

        fig = Figure(data=data, layout=layout)
        plotly.offline.plot(fig, 
                filename=os.path.join(results, 'throughput.html'))


def latency_hist(data_dict):
    print("Generating latency histogram...")
    data = []

    keys = sort_dict_keys(data_dict)

    for nservers, nclients in keys:
        x = data_dict[(nservers, nclients)]['latencies']['latency']

        data.append(Histogram(x=x,
                            name="%ds, %dcli" % (nservers, nclients),
                            opacity=0.6))

        layout = Layout(
                title='Latency histogram %s' % title, 
                showlegend=True,
                xaxis=dict(title='sendtime'),
                yaxis=dict(title='latency (usec)')
        )

        fig = Figure(data=data, layout=layout)
        plotly.offline.plot(fig, 
                filename=os.path.join(results, 'latency_hist.html'))


def latency_box(data_dict):
    print("Generating latency boxplot...")
    data = []

    keys = sort_dict_keys(data_dict)

    for nservers,nclients in keys:
        y = data_dict[(nservers, nclients)]['latencies']['latency']
        data.append(Box(y=y,
                            name="%ds, %dcli" % (nservers, nclients),
                            ))

        layout = Layout(
                title='Latency boxplot %s' % title,
                showlegend=False,
                xaxis=dict(title='nservers, nclients'),
                yaxis=dict(title='latency (usec)')
        )

        fig = Figure(data=data, layout=layout)
        plotly.offline.plot(fig, 
                filename=os.path.join(results, 'latency_box.html'))


# Throughput-latency
def throughput_latency(data_dict):
    print("Generating throughput-latency...")
    for dkey in data_dict: 
        data_dict[dkey]['latency_stat'] = data_dict[dkey]['latencies']['latency'].median()
        data_dict[dkey]['tput_stat'] = data_dict[dkey]['tputs'].mean()

    keys = sort_dict_keys(data_dict)

    x = [data_dict[key]['tput_stat'] for key in keys]  # throughout
    y = [data_dict[key]['latency_stat'] for key in keys] # latency

    data = Scatter(x=x,
            y=y,
            mode='lines+markers+text',
            textposition='top left',
            text=["%ds, %dcli" % (nserv, ncli) for nserv, ncli in keys],
            )

    layout = Layout(
            title="Mean throughput/median latency %s" % title,
            xaxis=dict(title='throughput (pps)'),
            yaxis=dict(title='latency (usec)'),
            )

    fig = Figure(data=[data], layout=layout)
    plotly.offline.plot(fig, 
            filename=os.path.join(results, 'tputlatency.html'))


def sort_dict_keys(data_dict):
    return sorted(data_dict.keys(), key=lambda elem: (elem[0], elem[1]))


def get_data_dict():
    print("Generating data dictionary...")
    for exp, exp_data in experiment_dict.items():
        nclients = exp_data['nclients']
        nservers = exp_data['nserver_threads']
        dkey = (nservers, nclients)

        latencies = pd.concat([x for x in exp_data['latency']], ignore_index=True)
        tputs = exp_data['throughput']['tput']

        if dkey not in data_dict:
            data_dict[dkey] = {}
            data_dict[dkey]['tputs'] = exp_data['throughput']['tput']
            data_dict[dkey]['latencies'] = latencies
        else:
            data_dict[dkey]['tputs'] = pd.concat([data_dict[dkey]['tputs'], 
                tputs], ignore_index=True)
            data_dict[dkey]['latencies'] = pd.concat([data_dict[dkey]['latencies'], 
                latencies], ignore_index=True)

    return data_dict


print("--------------------------------------------------------------------------------")
print("Generating plots!")
data_dict = get_data_dict()
throughput_latency(data_dict)
throughput()
latency_hist(data_dict)
latency_box(data_dict)
print("Done!")
print("--------------------------------------------------------------------------------\n")
