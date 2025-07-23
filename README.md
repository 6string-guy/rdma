# Follow these steps 

## Set up RDMA -
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev enp0s3

## For compilation use -

gcc -Wall -O2         client.c  -lrdmacm -libverbs -o client 
gcc -Wall  -O2 -pthread server.c  -lrdmacm -libverbs -o server

## Server 
./server -i <your ip> -p <port no.>
./client -i <your server ip> -p <port no.>



