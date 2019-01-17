sudo pip3 install colorlover

make mtcp 

echo "[`date +%s`] commit `git rev-parse HEAD`" >> results/README

cp generate_plot.py results
cp pickle_data.py results
cp experiment_batch_script.sh results

{
ntrials=3
nclient_threads=32

title="with increasing client thread count,<br>\
    $nclient_threads client threads/machine, $ntrials trial(s)"

nserver_threads=4
while [ $nserver_threads -le 32 ]; do

    nclient_machines=1
    while [ $nclient_machines -le 32 ]; do
        trial=1
        while [ $trial -le $ntrials ]; do
            echo Trial $trial: Running with $nclient_machines client machines, \
                $nserver_threads server threads, \
                $nclient_threads client threads
            python run_experiments.py theano "sudo ./NPmtcp" \
                --expduration 30 \
                --nclient_machines $nclient_machines \
                --nclient_threads $nclient_threads \
                --nserver_threads $nserver_threads
            echo
            ((trial=$trial+1))

        done
        ((nclient_machines=$nclient_machines*2))

    done

    ((nserver_threads=$nserver_threads*2))
done

nserver_threads=4
while [ $nserver_threads -le 32 ]; do

    nclient_machines=48
    while [ $nclient_machines -le 48 ]; do
        trial=1
        while [ $trial -le $ntrials ]; do
            echo Trial $trial: Running with $nclient_machines client machines, \
                $nserver_threads server threads, \
                $nclient_threads client threads
            python run_experiments.py theano "./NPtcp" \
                --expduration 30 \
                --nclient_machines $nclient_machines \
                --nclient_threads $nclient_threads \
                --nserver_threads $nserver_threads
            echo
            ((trial=$trial+1))

        done
        ((nclient_machines=$nclient_machines*2))

    done

    ((nserver_threads=$nserver_threads*2))
done

echo Experiments complete! Processing...

bash process_data.sh "$title"
echo All done!
} |& tee -a results/log.txt

