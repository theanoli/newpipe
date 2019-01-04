echo "[`date +%s`] commit `git rev-parse HEAD`" >> results/README

title="with increasing client thread count,<br>32 client threads/machine, 3 trials"

{
ntrials=1
counter=1
nclient_threads=32
nserver_threads=1

while [ $nserver_threads -le 32 ]; do

    nclient_machines=1
    while [ $nclient_machines -le 16 ]; do
        trial=0
        while [ $trial -lt $ntrials ]; do
            echo Trial $trial: Running with $nclient_machines client machines, \
                $nserver_threads server threads, \
                $nclient_threads client threads
            python run_experiments.py theano "./NPtcp" \
                --expduration 30 \
                --unpin_sthreads \
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

