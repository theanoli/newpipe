sudo pip3 install colorlover

{
#cmd="./NPtcp"
#make tcp

#cmd="sudo ./NPmtcp"
#make mtcp

cmd="./NPudp"
make udp

ret=$?
if [[ $ret != 0 ]]; then
    echo "Compile failed!"
    exit -1
fi

echo "[`date +%s`] commit `git rev-parse HEAD`" >> results/README

cp generate_plot.py results
cp pickle_data.py results
cp experiment_batch_script.sh results

ntrials=1

nclient_ports_min=1
nclient_ports_max=1

nserver_threads_min=2
nserver_threads_max=8

nclient_threads_min=16
nclient_threads_max=16

nclient_machines_min=2
nclient_machines_max=8

title="with UDP, increasing number of clients<br>\
    1 server machine, 8 client threads per machine, 1 trials"

nserver_threads=$nserver_threads_min
while [ $nserver_threads -le $nserver_threads_max ]; do

    nclient_machines=$nclient_machines_min
    while [ $nclient_machines -le $nclient_machines_max ]; do

        nclient_threads=$nclient_threads_min
        while [ $nclient_threads -le $nclient_threads_max ]; do

            nclient_ports=$nclient_ports_min
            while [ $nclient_ports -le $nclient_ports_max ]; do
                trial=1
                while [ $trial -le $ntrials ]; do
                    echo Trial $trial: Running with $nclient_machines client machines, \
                        $nserver_threads server threads, \
                        $nclient_threads client threads, \
                        $nclient_ports ports per client thread
                    python run_experiments.py theano "$cmd" \
                        --expduration 30 \
                        --nclient_machines $nclient_machines \
                        --nclient_threads $nclient_threads \
                        --nserver_threads $nserver_threads \
                        --nclient_ports $nclient_ports
                    echo
                    ((trial=$trial+1))

                done
                ((nclient_ports=$nclient_ports*2))

            done
            ((nclient_threads=$nclient_threads*2))

        done
        ((nclient_machines=$nclient_machines*2))

    done
    ((nserver_threads=$nserver_threads*2))
done

echo Experiments complete! Processing...

rm -rf log_*

bash process_data.sh "$title"
echo All done!
} |& tee -a results/log.txt

