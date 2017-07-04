
#include <iostream>

#include <storage/SystemInfo/SystemInfo.h>

using namespace std;
using namespace storage;


void
test_blkid(SystemInfo& system_info)
{
    try
    {
	const Blkid& blkid = system_info.getBlkid();
	cout << "Blkid success" << endl;
	cout << blkid << endl;
    }
    catch (const exception& e)
    {
	cerr << "Blkid failed" << endl;
    }
}


int
main()
{
    set_logger(get_logfile_logger());

    SystemInfo system_info;

    test_blkid(system_info);
}
