echo "[`date +%s`] commit `git rev-parse HEAD`" >> results/README

{
ntrials=1
counter=1
while [ $counter -le 16 ]; do

    trial=0
    while [ $trial -lt $ntrials ]; do
        echo Running with $counter client machines.
        python run_experiments.py theano "./NPtcp" --expduration 30 --nclient_machines $counter --nclient_threads 32 --nserver_threads 16
        echo Done with $counter server machines.
        echo
        ((trial=$trial+1))
    done
    
    ((counter=$counter*2))
done
echo Experiments complete! Processing...

bash process_data.sh "with increasing client machine count,<br>16 server threads, 32 client threads, 1 trial"
echo All done!
} |& tee -a results/log.txt
