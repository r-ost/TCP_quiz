Ponizej przykladowe uzycie:

SERWERY:
Uruchamiamy dwa serwery (jeden z krotkimi, a drugi z dlugimi pytaniami).
Dzieki temu bedzie mozna zaobserwowac wysylanie znaku '0' do serwera i zmiane pytania.

$ ./server localhost 9000 2 krotkie_pytania.txt
$ ./server localhost 9001 3 dlugie_pytania.txt

KLIENT
Laczymy sie z dwoma serwerami.

$ ./client localhost 9000 localhost 9001