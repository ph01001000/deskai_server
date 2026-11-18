package main

import (
	"fmt"
	"log"
	"net"
	"sync"
	// "error"
	"deskai/protocol"
)

var syncpool = sync.Pool{
	New: func() any {
		b:=make([]byte,1500)
		return &b },
}


func main() {
	addr:= &net.UDPAddr{
		Port: 8000,
		IP: net.ParseIP("0.0.0.0"),
	}

	conn,err := net.ListenUDP("udp",addr)
	if err!=nil{
		log.Fatalf("listener not started: %v",err) 
	}
	defer conn.Close()

	fmt.Printf("UDP server listening on 8000: \n")
	
	for{
		bufPtr:= syncpool.Get().(*[]byte)
		n, Addr, err := conn.ReadFromUDP(*bufPtr)
		if err!=nil{
			fmt.Println(err)
			syncpool.Put(bufPtr)
			continue
		}

		header, _, err:= protocol.DecodePacket((*bufPtr)[:n])
		if err!=nil{
			fmt.Println(err)
		}else{
			fmt.Printf("%s, %d, %d ,%d \n",Addr,header.Seq,header.Msgtype,header.Length)
		}
		syncpool.Put(bufPtr)
	}
}