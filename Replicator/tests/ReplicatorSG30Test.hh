//
// Created by Callum Birks on 18/01/2023.
//

#pragma once

#include "ReplicatorCollectionSGTest.hh"

class ReplicatorSG30Test : public ReplicatorCollectionSGTest {
  public:
    ReplicatorSG30Test() : ReplicatorCollectionSGTest("3.0", "3.1", 4884) {}
};