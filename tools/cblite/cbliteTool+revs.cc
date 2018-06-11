//
// cbliteTool+revs.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "cbliteTool.hh"


void CBLiteTool::revsUsage() {
    writeUsageCommand("revs", false, "DOCID");
    cerr <<
    "  Shows a document's revision history\n"
    "    --remotes : Shows which revisions are known current on remote databases\n"
    "  Revision flags are denoted by dashes or the letters:\n"
    "    [D]eleted  [X]Closed  [C]onflict  [A]ttachments  [K]eep body  [L]eaf"
    ;
}


void CBLiteTool::revsInfo() {
    // Read params:
    processFlags(kRevsFlags);
    if (_showHelp) {
        revsUsage();
        return;
    }
    openDatabaseFromNextArg();
    string docID = nextArg("document ID");
    endOfArgs();

    auto doc = readDoc(docID);
    if (!doc)
        return;

    cout << "Document \"" << ansiBold() << doc->docID << ansiReset() << "\"";
    if (doc->flags & kDocDeleted)
        cout << ", Deleted";
    if (doc->flags & kDocConflicted)
        cout << ", Conflicted";
    if (doc->flags & kDocHasAttachments)
        cout << ", Has Attachments";
    cout << "\n";

    // Collect remote status:
    RemoteMap remotes;
    if (_showRemotes) {
        for (C4RemoteID remoteID = 1; true; ++remoteID) {
            alloc_slice revID(c4doc_getRemoteAncestor(doc, remoteID));
            if (!revID)
                break;
            remotes.emplace_back(revID);
        }
    }

    // Collect revision tree info:
    RevTree tree;
    alloc_slice root; // use empty slice as root of tree

    do {
        alloc_slice leafRevID(doc->selectedRev.revID);
        alloc_slice childID = leafRevID;
        while (c4doc_selectParentRevision(doc)) {
            alloc_slice parentID(doc->selectedRev.revID);
            tree[parentID].insert(childID);
            childID = parentID;
        }
        tree[root].insert(childID);
        c4doc_selectRevision(doc, leafRevID, false, nullptr);
    } while (c4doc_selectNextLeafRevision(doc, true, true, nullptr));

    writeRevisionChildren(doc, tree, remotes, root, 1);

    for (C4RemoteID i = 1; i <= remotes.size(); ++i) {
        alloc_slice addr(c4db_getRemoteDBAddress(_db, i));
        if (!addr)
            break;
        cout << "[REMOTE#" << i << "] = " << addr << "\n";
    }
}


void CBLiteTool::writeRevisionTree(C4Document *doc,
                                   RevTree &tree,
                                   RemoteMap &remotes,
                                   alloc_slice root,
                                   int indent)
{
    C4Error error;
    if (!c4doc_selectRevision(doc, root, true, &error))
        fail("accessing revision", error);
    auto &rev = doc->selectedRev;
    cout << string(indent, ' ');
    cout << "* ";
    if ((rev.flags & kRevLeaf) && !(rev.flags & kRevClosed))
        cout << ansiBold();
    cout << rev.revID << ansiReset();

    int pad = max(0, 50 - int(indent + 2 + rev.revID.size));
    cout << string(pad, ' ');

    if (rev.flags & kRevClosed)
        cout << 'X';
    else
        cout << ((rev.flags & kRevDeleted)    ? 'D' : '-');
    cout << ((rev.flags & kRevIsConflict)     ? 'C' : '-');
    cout << ((rev.flags & kRevClosed)         ? 'X' : '-');
    cout << ((rev.flags & kRevHasAttachments) ? 'A' : '-');
    cout << ((rev.flags & kRevKeepBody)       ? 'K' : '-');
    cout << ((rev.flags & kRevLeaf)           ? 'L' : '-');

    cout << " #" << rev.sequence;
    if (rev.body.buf) {
        cout << ", ";
        writeSize(rev.body.size);
    }

    if (root == slice(doc->revID))
        cout << ansiBold() << "  [CURRENT]" << ansiReset();

    C4RemoteID i = 1;
    for (alloc_slice &remote : remotes) {
        if (remote == root)
            cout << "  [REMOTE#" << i << "]";
        ++i;
    }

    cout << "\n";
    writeRevisionChildren(doc, tree, remotes, root, indent+2);
}

void CBLiteTool::writeRevisionChildren(C4Document *doc,
                           RevTree &tree,
                                       RemoteMap &remotes,
                           alloc_slice root,
                           int indent)
{
    auto &children = tree[root];
    for (auto i = children.rbegin(); i != children.rend(); ++i) {
        writeRevisionTree(doc, tree, remotes, *i, indent);
    }
}


