echo "[`date +%s`] commit `git rev-parse HEAD`" >> results/README

{
ntrials=5
counter=1
while [ $counter -le 8 ]; do

    trial=0
    while [ $trial -lt $ntrials ]; do
        echo Running with $counter client machines.
        python run_experiments.py theano "./NPtcp" --expduration 60 --nclient_machines $counter --nclient_threads 64 --nserver_threads 64
        echo Done with $counter client machines.
        echo
        ((trial=$trial+1))
    done
    
    ((counter=$counter*2))
done
echo Experiments complete! Processing...

process_data.sh
echo All done!
} |& tee -a results/log.txt
