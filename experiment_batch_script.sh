sudo pip3 install colorlover

make mtcp
ret=$?
if [[ $ret != 0 ]]; then
    echo "Compile failed!"
    exit -1
fi

echo "[`date +%s`] commit `git rev-parse HEAD`" >> results/README

cp generate_plot.py results
cp pickle_data.py results
cp experiment_batch_script.sh results

{
ntrials=3

nserver_threads_min=1
nserver_threads_max=16

nclient_threads_min=1
nclient_threads_max=1

nclient_machines_min=1
nclient_machines_max=1

title="with increasing server thread count,<br>\
    $nclient_threads_min client threads (1 client machine), $ntrials trial(s)"

nserver_threads=$nserver_threads_min
while [ $nserver_threads -le $nserver_threads_max ]; do

    nclient_machines=$nserver_threads
    nclient_machines_max=$nserver_threads
    while [ $nclient_machines -le $nclient_machines_max ]; do

        nclient_threads=$nclient_threads_min
        while [ $nclient_threads -le $nclient_threads_max ]; do

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

