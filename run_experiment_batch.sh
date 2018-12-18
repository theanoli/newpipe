
{
trial=0
while [ $trial -le 3 ]; do
    counter=1
    while [ $counter -le 32 ]; do
        echo Running with $counter client threads per machine.
        python run_experiments.py theano "./NPtcp" --expduration 60 --nclient_machines 16 --nclient_threads 32 --nserver_threads $counter
        echo Done with $counter client threads per machine.
        echo
        ((counter=$counter*2))
    done
    
    ((trial=$trial+1))
done
} |& tee -a results/log.txt
