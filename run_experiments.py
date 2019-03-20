#! /usr/bin/python

import os
import re
import sys

import atexit
import argparse
import math
import shlex
import subprocess
import time

default_expduration = 60
default_nclient_threads = 256

class Experiment(object):
    def __init__(self, args):
        self.printlabel = "[" + os.path.basename(__file__) + "]"

        self.basecmd = args.basecmd
        self.whoami = args.whoami
        self.server_addr = args.server_addr
        self.nclient_threads = args.nclient_threads
        self.nclient_machines = args.nclient_machines
        self.nclient_ports = args.nclient_ports
        self.nserver_threads = args.nserver_threads
        self.start_port = args.start_port
        self.wdir = args.wdir

        self.outfile = args.outfile
        self.outdir = args.outdir

        self.collect_stats = args.collect_stats
        self.expduration = args.expduration
        self.unpin_sthreads = args.unpin_sthreads
        self.unpin_cthreads = args.unpin_cthreads
        self.use_ctrlip = args.use_ctrlip

        self.machine_dict, self.clients, self.servers = self.read_machine_info()
        self.clients = self.clients[:self.nclient_machines]

        timestamp = int(time.time())
        if self.outfile == None:
            self.outfile = "results"

        if self.outdir == None:
            self.outdir = os.path.join(self.wdir, "results", "results-%d" % 
                    timestamp)

        if not os.path.isdir(self.outdir):
            os.mkdir(self.outdir)

        self.printer("Number of client threads: %d" % 
                (self.nclient_threads * len(self.clients)))

        self.total_clients = self.nclient_threads * len(self.clients)

        # Dump all the class variables (experiment settings) to file
        with open("%s/config.txt" % self.outdir, 'w') as f:
            for var, value in self.__dict__.iteritems():
                f.write("%s: %s\n" % (var, value))
        
        self.kill_zombie_processes()
        atexit.register(self.kill_zombie_processes)
        
    def printer(self, msg): 
        print("%s %s" % (self.printlabel, msg))
    
    def kill_zombie_processes(self):
        # Clean up any potential zombies on the client side(s)
        self.printer("Killing zombie processes on client(s).")
        killers = []
        for machine in self.machine_dict.keys(): 
            killers.append(subprocess.Popen(
                shlex.split("ssh -p 22 %s@%s.emulab.net 'sudo pkill \"NP[a-z]*\"'" % 
                        (self.whoami, self.machine_dict[machine]['machineid']))))
        for killer in killers:
            killer.wait()

    def read_machine_info(self):
        clients = []
        servers = []
        machines = {}

        f = open("../machine_info.txt", "r")

        for line in f.readlines():
            fields = line.strip().split(',')
            machinename = fields[0]
            machineid = fields[1]
            iface = fields[2]
            mac = fields[3]
            ip = fields[4]
            ctrl_ip = fields[5]

            if "server" in machinename:
                servers.append(machinename)
            else:
                clients.append(machinename)
            
            machines[machinename] = {
                    'machineid': machineid,
                    'iface': iface,
                    'mac': mac,
                    'ip': ip,
                    'ctrl_ip': ctrl_ip
            }

        f.close()
        return machines, sorted(clients), sorted(servers)

    def launch_server(self):
        # Launch the server-side program and then wait a bit 
        self.printer("Launching server...")
        
        serv_cmd = (self.basecmd +
                # -c should be the per-server thread #clients
                " -c %d" % self.total_clients +  
                " -P %d" % self.start_port +
                " -d %s" % self.outdir + 
                " -o %s" % self.outfile +
                " -u %d" % self.expduration + 
                " -T %d" % self.nserver_threads + 
                " -p 0")
        if self.collect_stats:
            serv_cmd += " -l"
        if not self.unpin_sthreads:
            serv_cmd += " -i"
        if self.use_ctrlip:
            serv_cmd += " -m %s" % self.machine_dict['server-0']['ctrl_ip']

        ssh = "ssh -p 22 %s@%s.emulab.net 'cd %s; %s;'" % (self.whoami,
                self.machine_dict['server-0']['machineid'], self.wdir, serv_cmd)
        self.printer("Launching server process: %s" % serv_cmd)
        sys.stdout.flush()
        server = subprocess.Popen(shlex.split(ssh))
        return server

    def launch_clients(self):
        # Launch the client-side programs
        self.printer("Launching clients...")
        subprocesses = []

        for client in self.clients:
            cmd = (self.basecmd + 
                    " -c %d" % self.nserver_threads + 
                    " -d %s" % self.outdir + 
                    " -o %s" % self.outfile +
                    " -u %d" % self.expduration + 
                    " -T %d" % self.nclient_threads +
                    " -p %d" % self.nclient_ports + 
                    " -P %d" % self.start_port)
            if not self.unpin_cthreads:
                cmd += " -i"
            if self.use_ctrlip:
                cmd += (" -m %s" % self.machine_dict[client]['ctrl_ip'] + 
                        " -H %s" % self.machine_dict['server-0']['ctrl_ip'])
            else:
                cmd += " -H %s" % self.machine_dict[self.server_addr]['ip']

            ssh = "ssh -p 22 %s@%s.emulab.net 'cd %s; %s'" % (self.whoami, 
                    self.machine_dict[client]['machineid'], self.wdir, cmd)
            self.printer("Sending command to client %s: %s" % (client, ssh))
            sys.stdout.flush()
            p = subprocess.Popen(shlex.split(ssh))
            subprocesses.append(p)
        return subprocesses
 
    
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Run one or more server processes '
            'and multiple client processes.')
    parser.add_argument('whoami',
            help=('Emulab username for SSH'))
    parser.add_argument('basecmd',
            help=('Command to run for each process. Will be augmented '
            'with port number and total number of clients for server, '
            'server IP and port number for clients.'))
    parser.add_argument('--server_addr',
            help=('Name of the server. Default server-0'),
            default="server-0") 
    parser.add_argument('--nclient_threads',
            type=int,
            help=('Number of client threads to run per client machine. '
                'Default %d.' % default_nclient_threads),
            default=default_nclient_threads)
    parser.add_argument('--nclient_machines',
            type=int,
            help=("Number of client machines to launch. Default 1."),
            default=1)
    parser.add_argument('--nserver_threads',
            type=int,
            help='Number of server threads to run. Default 1.',
            default=1)
    parser.add_argument('--start_port',
            type=int,
            help='Base port number. Default 8000.',
            default=8000)
    parser.add_argument('--wdir', 
            help='Working directory. Default /proj/sequencer/tsch/newpipe.',
            default='/proj/sequencer/tsch/newpipe')
    parser.add_argument('--outdir',
            help=('Directory to write results to.'),
            default=None)
    parser.add_argument('--collect_stats',
            help=('Collect stats about CPU usage.'),
            action='store_true')
    parser.add_argument('--expduration',
            type=int,
            help=('Set throughput experiment duration. Default %ds.' %
                default_expduration),
            default=default_expduration)
    parser.add_argument('--unpin_sthreads',
            help=('Don\'t pin server threads to cores.'),
            action='store_true')
    parser.add_argument('--unpin_cthreads',
            help=('Don\'t pin client threads to cores.'),
            action='store_true')
    parser.add_argument('--outfile',
            help=('Base name for experiments'),
            default=None)
    parser.add_argument('--nclient_ports',
            help=('How many connections to open per client thread'),
            type=int,
            default=1)
    parser.add_argument('--use_ctrlip',
            help=('Whether to include the control IP address for client machines',
                ' (used in eRPC only for now)'),
            action='store_true')

    args = parser.parse_args()

    experiment = Experiment(args)
    experiment.printer("Running experiment!")
    experiment.kill_zombie_processes()
    server = experiment.launch_server()
    clients = experiment.launch_clients()
    server.wait()
    experiment.printer("Completed experiment!")

