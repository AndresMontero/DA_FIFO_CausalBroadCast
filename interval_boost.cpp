#include <bits/stdc++.h>
#include <boost/icl/interval_set.hpp>

using namespace std;
using namespace boost::icl;

int main(){
    interval<int>::type message1 = interval<int>::closed(1,5);
    interval<int>::type message2 = interval<int>::closed(5,10);
    interval<int>::type message3 = interval<int>::closed(12,18);
    interval_set<int> packets;
    packets.add(message1);
    packets.add(message2);
    packets.add(message3);
    for(auto it = packets.begin(); it != packets.end() ; it++ )
        cout << it->lower() << " " << it->upper() << endl;
        
    return 0;
}