sudo pip3 install colorlover

echo "[`date +%s`] commit `git rev-parse HEAD`" >> results/README

cp generate_plot.py results
cp pickle_data.py results
cp experiment_batch_script.sh results

{
ntrials=3
counter=1
nclient_threads=16
nserver_threads=4

title="with increasing client thread count,<br>$nserver_threads server threads/machine, \
    $nclient_threads client threads/machine, $ntrials trial(s)"

while [ $nserver_threads -le 128 ]; do

    nclient_machines=1
    while [ $nclient_machines -le 1 ]; do
        trial=0
        while [ $trial -lt $ntrials ]; do
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

