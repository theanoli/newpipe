
counter=1
while [ $counter -le 16 ]; do
    echo Running with $counter client threads per machine.
    python run_experiments.py theano "./NPtcp" --expduration 30 --nclient_machines 1 --nclient_threads $counter 
    echo Done with $counter client threads per machine.
    echo
    ((counter=$counter*2))
done
