#include <cassert>
#include <vector>
#include <cstdint>
#include <iostream>
#include "hashll.h"
#include <assert.h>
// -----------------------------------------------------------------------
// Testing checklist:
// Same bucket nodes        o
// Different bucket nodes   o
// Searching                o
// Adding                   o
// Removing                 o
// Searching                o
// Marking as recent        o
// -----------------------------------------------------------------------

void initialize_test_structure(HASHLL::HashLL &samebucket, HASHLL::HashLL &diffbucket)
{
    for (int i = 0; i < 11; i++)
    {
        samebucket.touch(0x1000 + i);  // all map to vp_num = 1
    }


    for (int i = 1; i < 20; i++)
    {
        diffbucket.touch(i * 4096); 
    }
}



// -----------------------------------------------------------------------
// Execute tests
// -----------------------------------------------------------------------
int main()
{
    HASHLL::HashLL samebucket(10);
    HASHLL::HashLL diffbucket(10);
    initialize_test_structure(samebucket, diffbucket);

    std::vector<uint64_t> samebucket_p_num = samebucket.get_nodes();
    std::vector<uint64_t> diffbucket_p_num = diffbucket.get_nodes();

    for (int i = 0; i < samebucket_p_num.size(); i++)
    {
        std::cout << samebucket_p_num[i] << "sb ";
    }
    std::cout << "sbcap " << samebucket.get_cap() << " sbsize " << samebucket.get_size();
    std::cout << std::endl;


    for (int i = 0; i < diffbucket_p_num.size(); i++)
    {
        std::cout << diffbucket_p_num[i] << "db ";
    }
    std::cout << "dbcap " << diffbucket.get_cap() << " dbsize " << diffbucket.get_size();
    std::cout << std::endl;
    return 0;
}