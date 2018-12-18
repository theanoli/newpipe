import os
import re
import sys
import math
import pandas as pd
import numpy as np
import copy
import pickle
import subprocess
import multiprocessing

start_cutoff_duration = 5  # in seconds
end_cutoff_duration = 1
us_per_sec = 1e6

def load_latency_file(fpath):
    # Returns a dataframe
    df = pd.read_csv(fpath, 
                     index_col=False,
                     names=["send_sec", "send_ns", "recv_sec", "recv_ns"],
                    sep=',')
    return df

def load_tput_file(fpath):
    df = pd.read_csv(fpath,
            index_col=False,
            names=["ts_sec", "ts_ns", "pkt_count"],
            sep=',')
    return df

def sec_to_usec(sec_string):
    return int(sec_string) * 1e6

def nsec_to_usec(nsec_string):
    return int(nsec_string) * 1e-3

def process_latency_timestamps(df):
    sendtime = df['send_sec'].apply(lambda x: sec_to_usec(x)) + df['send_ns'].apply(lambda x: nsec_to_usec(x))
    recvtime = df['recv_sec'].apply(lambda x: sec_to_usec(x)) + df['recv_ns'].apply(lambda x: nsec_to_usec(x))
    cols = ["sendtime", "recvtime", "latency"]
    newdf = pd.concat([sendtime, recvtime, recvtime - sendtime],
                    axis=1)
    newdf.columns = cols
    return newdf

def process_tput_dataframe(df):
    timestamp = df['ts_sec'].apply(lambda x: sec_to_usec(x)) + df['ts_ns'].apply(lambda x: nsec_to_usec(x))
    tput = df['pkt_count'].diff()
    cols = ['timestamp', 'tput']
    newdf = pd.concat([timestamp, tput],
            axis=1)
    newdf.columns = cols
    return newdf.dropna()

def load_client_data(experiment):
    path_base = os.path.join(home, results_dir, experiment)
    files = os.listdir(path_base)
    latency_files = list(filter(lambda x: "latency.dat" in x, files))
    tput_files = list(filter(lambda x: "throughput.dat" in x, files))
    
    if len(tput_files) > 1:
        print("Found more than 1 throughput file, FYI... skipping others")
    tput_file = tput_files[0]

    df = load_tput_file(os.path.join(path_base, tput_file))
    try:
        tput = process_tput_dataframe(df)
        starttime = tput['timestamp'].iloc[0]
        start_cutoff = starttime + start_cutoff_duration * us_per_sec
        endtime = tput['timestamp'].iloc[-1]
        end_cutoff = endtime - end_cutoff_duration * us_per_sec
        tput = tput[tput['timestamp'].between(start_cutoff, end_cutoff)]

        # Set start time as earliest marked time, convert to usec
        new_starttime = tput['timestamp'].iloc[0]
        tput['timestamp'] = tput['timestamp'].apply(lambda x: (x - new_starttime)/us_per_sec)
    except:
        # Need to investigate what is wrong with this file; do not continue
        print("%s: derped on file %s" % (experiment, tput_file))
        raise

    latencies = []
    for fname in latency_files:
        df = load_latency_file(os.path.join(path_base, fname))
        try:
            latency = process_latency_timestamps(df)
            starttime = latency['sendtime'].iloc[0]
            start_cutoff = starttime + start_cutoff_duration * us_per_sec
            endtime = latency['sendtime'].iloc[-1]
            end_cutoff = endtime - end_cutoff_duration * us_per_sec
            latency = latency[latency['sendtime'].between(start_cutoff, end_cutoff)]
            new_starttime = latency['sendtime'].iloc[0]
            
            # Set start time as earliest marked time, convert to usec
            latency['sendtime'] = latency['sendtime'].apply(lambda x: (x - new_starttime)/us_per_sec)
            latency['recvtime'] = latency['recvtime'].apply(lambda x: (x - new_starttime)/us_per_sec)
            latencies.append(latency[['latency', 'sendtime']])
        except:
            # Need to investigate what is wrong with this file; do not continue
            print("%s: derped on file %s" % (experiment, fname))
            raise
    
    #return pd.concat(latencies, ignore_index=True), tputs
    return latencies, tput

def load_config(experiment):
    settings = {}
    with open(os.path.join(home, results_dir, experiment, 'config.txt')) as f:
        for line in f.readlines():
            line = line.strip().split(": ")
            key = line[0]
            val = (": ").join(line[1:])
            settings[key] = val
    return settings

def worker(experiment, return_dict):
    print("Loading experiment " + experiment)

    # Load/process data
    latencies, tput = load_client_data(experiment)

    # Load/process config information
    config = load_config(experiment)

    return_dict[experiment] = {'latency': latencies,#.mean(),
                          'throughput': tput,
                          'config': config,
                          'nclients': int(config['total_clients']),
                          'nclient_threads': int(config['nclient_threads']),
                          'nserver_threads': int(config['nserver_threads']),
                          }
    print("Completed loading experiment %s" % experiment)
    print("\tnclient_threads, total %d client threads, %d nserver_threads %d" % (
        return_dict[experiment]['nclient_threads'],
        return_dict[experiment]['nclients'],
        return_dict[experiment]['nserver_threads']))

if __name__ == '__main__':
    print("--------------------------------------------------------------------------------")
    print("Pickler is getting ready to pickle...")
    home = os.getcwd()
    try: 
        results_dir = sys.argv[1]
    except:
        results_dir = 'results'

    try:
        with open(os.path.join(home, results_dir, 'experiment_dict.pickle'), 'r+b') as f:
            experiment_dict = pickle.load(f)
    except:
        experiment_dict = {}

    experiments = list(filter(lambda x: 'results' in x, os.listdir(os.path.join(home, results_dir))))

    print("\nOld experiment_dict keys:")
    [print(key) for key in experiment_dict.keys()]

    manager = multiprocessing.Manager()
    return_dict = manager.dict()

    jobs = []
    for experiment in experiments:
        if experiment not in experiment_dict.keys():
            p = multiprocessing.Process(target=worker, args=(experiment,
                return_dict))
            jobs.append(p)
            p.start()

    for p in jobs:
        p.join()
    
    experiment_dict.update(return_dict)
    print("\nNew experiment_dict keys:")
    [print(key) for key in experiment_dict.keys()]

    print("\nPickling!")
    with open(os.path.join(home, results_dir, 'experiment_dict.pickle'), 'w+b') as f:
        pickle.dump(experiment_dict, f)

    print("--------------------------------------------------------------------------------\n")
