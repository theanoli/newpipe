import os
import re
import sys
import math
import pandas as pd
import numpy as np
import copy
import pickle
import subprocess

import colorlover as cl
import plotly
import plotly.plotly as py
from plotly.offline import download_plotlyjs, init_notebook_mode, iplot
from plotly.graph_objs import *
#init_notebook_mode()

title = sys.argv[1]
try:
    results_dir = sys.argv[2]
except:
    results_dir = 'results'

home = os.getcwd()
results = os.path.join(home, results_dir)

with open(os.path.join(results, 'experiment_dict.pickle'), 'rb') as f:
    experiment_dict = pickle.load(f)

data_dict = {}

def throughput():
    print("Generating throughput over time...")

    data = []
    i = 0
    for exp, exp_data in experiment_dict.items():
        color = cl.scales['12']['qual']['Paired'][i % 12]
        nserver_threads = exp_data['nserver_threads']
        nclients = exp_data['nclients']

        x = exp_data['server_tput']['timestamp']
        y = exp_data['server_tput']['tput']

        data.append(Scatter(x=x,
            y=y,
            legendgroup=exp,
            name="%ds, %dcli" % (nserver_threads, nclients),
            line=dict(color=color),
            mode='lines+markers',))

        for client_tput in exp_data['client_tputs']:
            x = client_tput['timestamp']
            y = client_tput['tput']
        
            data.append(Scatter(x=x,
                y=y,
                legendgroup=exp,
                line=dict(color=color),
                name="%ds, %dcli" % (nserver_threads, nclients),
                mode='lines+markers',))
        i += 1

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

    grouped_data_dict = {}

    for (nservers, nclients) in keys: 
        if nservers not in grouped_data_dict:
            grouped_data_dict[nservers] = {'x': [], 'y': [], 'nclients': []}
        grouped_data_dict[nservers]['x'].append(data_dict[(nservers, nclients)]['tput_stat'])
        grouped_data_dict[nservers]['y'].append(data_dict[(nservers, nclients)]['latency_stat'])
        grouped_data_dict[nservers]['nclients'].append(nclients)

    data = []
    for nservers, grouped_data in grouped_data_dict.items():
        data.append(Scatter(x=grouped_data['x'],
            y=grouped_data['y'],
            mode='lines+markers',
            name='%s servers' % nservers,
            ))

    layout = Layout(
            title="Mean throughput/median latency %s" % title,
            xaxis=dict(title='throughput (pps)'),
            yaxis=dict(title='latency (usec)'),
            )

    fig = Figure(data=data, layout=layout)
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
        tputs = exp_data['server_tput']['tput']

        if dkey not in data_dict:
            data_dict[dkey] = {}
            data_dict[dkey]['tputs'] = tputs
            data_dict[dkey]['latencies'] = latencies
        else:
            data_dict[dkey]['tputs'] = pd.concat([data_dict[dkey]['tputs'], 
                tputs], ignore_index=True)
            data_dict[dkey]['latencies'] = pd.concat([data_dict[dkey]['latencies'], 
                latencies], ignore_index=True)

    return data_dict


print("--------------------------------------------------------------------------------")
print("Generating plots with title: \"%s\"" % title)
data_dict = get_data_dict()
throughput_latency(data_dict)
throughput()
#latency_hist(data_dict)
latency_box(data_dict)
print("Done!")
print("--------------------------------------------------------------------------------\n")
