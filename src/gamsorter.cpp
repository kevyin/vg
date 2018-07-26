#include "gamsorter.hpp"
#include "utility.hpp"
#include "json2pb.h"

/**
 * \file gamsorter.cpp
 * GAMSorter: sort a gam by position and offset.
 * Store unmapped reads at node 0.
 */

using namespace std;
using namespace vg;


void GAMSorter::sort(vector<Alignment>& alns) const {
    std::sort(alns.begin(), alns.end(), [&](const Alignment& a, const Alignment& b) {
        return this->less_than(a, b);
    });
}

void GAMSorter::dumb_sort(istream& gam_in, ostream& gam_out) const {
    std::vector<Alignment> sort_buffer;

    stream::for_each<Alignment>(gam_in, [&](Alignment &aln) {
        sort_buffer.push_back(aln);
    });

    this->sort(sort_buffer);
    
    // Write the output in non-enormous chunks, so indexing is actually useful
    vector<Alignment> out_buffer;
    
    for (auto& aln : sort_buffer) {
        out_buffer.push_back(aln);
        stream::write_buffered(gam_out, out_buffer, 1000);
    }
    stream::write_buffered(gam_out, out_buffer, 0);
}

void GAMSorter::stream_sort(istream& gam_in, ostream& gam_out) const {

    // Read the input into buffers.
    std::vector<Alignment> input_buffer;
    
    // When a buffer is full, sort it and write it to a temporary file, which
    // we remember.    
    vector<string> temp_file_names;
    
    auto finish_buffer = [&]() {
        // Do a sort. TODO: do it on another thread.
        this->sort(input_buffer);
        
        // Save it to a temp file.
        string temp_name = temp_file::create();
        temp_file_names.push_back(temp_name);
        ofstream temp_stream(temp_name);
        // OK to save as one massive group here.
        // TODO: This write could also be in a thread.
        stream::write_buffered(temp_stream, input_buffer, 0);
        
        input_buffer.clear();
    };
    
    
    stream::for_each<Alignment>(gam_in, [&](const Alignment& aln) {
        // Buffer each input alignment
        input_buffer.push_back(aln);
        if (input_buffer.size() == max_buf_size) {
            // We have a full temp file's worth of data.
            finish_buffer();
        }
    });
    finish_buffer();
    
    
    // Put all the files in a priority queue based on which has an alignment that comes first.
    // We work with pointers to cursors because we don't want to be copying the actual cursors around the heap.
    // We also *reverse* the order, because priority queues put the "greatest" element forts
    using cursor_t = stream::ProtobufIterator<Alignment>;
    auto cursor_order = [&](cursor_t*& a, cursor_t*& b) {
        if (b->has_next()) {
            if(!a->has_next()) {
                // Cursors that aren't empty come first
                return true;
            }
            return less_than(*(*b), *(*a));
        }
        return false;
    };
    priority_queue<cursor_t*, vector<cursor_t*>, decltype(cursor_order)> temp_files(cursor_order);
    
    // The open files also need to live in a collection; the cursors don't own them
    // They also can't be allowed to move since we reference them
    list<ifstream> temp_ifstreams;
    // The cursors also need to live in a collection, because we don't want to be
    // moving/copying them and their internal buffers and streams.
    // And they can't move after creation either.
    list<cursor_t> temp_cursors;
    
    for (auto& name : temp_file_names) {
        // Open each file again
        temp_ifstreams.emplace_back();
        temp_ifstreams.back().open(name);
        cerr << "Open temp file " << name << endl;
        // Make a cursor for it and put it in the heap
        temp_cursors.emplace_back(temp_ifstreams.back());
        
        if (!temp_cursors.back().has_next()) {
            cerr << "\tFile is empty!" << endl;
        }
    
        // Put the cursor pointer in the queue
        temp_files.push(&temp_cursors.back());
    }
    
    cerr << "Going to merge from " << temp_files.size() << " cursors" << endl;
    
    vector<Alignment> output_buffer;
    while(!temp_files.empty() && temp_files.top()->has_next()) {
        // Until we have run out of data in all the temp files
        
        // Pop off the winning cursor
        cursor_t* winner = temp_files.top();
        temp_files.pop();
        
        cerr << "Winning cursor " << winner << endl;
        
        // Grab and emit its alignment
        output_buffer.push_back(*(*winner));
        stream::write_buffered(gam_out, output_buffer, 1000);
        
        // Advance it
        winner->get_next();
        
        // Put it back in the heap if it is not depleted
        if (winner->has_next()) {
            temp_files.push(winner);
        }
        // TODO: Maybe keep it off the heap for the next loop somehow if it still wins
    }
    
    // Finish off the output
    stream::write_buffered(gam_out, output_buffer, 0);
    
    // The temp files will get cleaned up automatically.
}

bool GAMSorter::less_than(const Alignment &a, const Alignment &b) const {
    return less_than(get_min_position(a), get_min_position(b));
}

Position GAMSorter::get_min_position(const Alignment& aln) const {
    return get_min_position(aln.path());
}

Position GAMSorter::get_min_position(const Path& path) const {
    if (path.mapping_size() == 0) {
        // This path lives at a default Position
        return Position();
    }
    
    Position min = path.mapping(0).position();
    for(size_t i = 1; i < path.mapping_size(); i++) {
        const Position& other = path.mapping(i).position();
        if (less_than(other, min)) {
            // We found a smaller position
            min = other;
        }
    }
    
    return min;
}

bool GAMSorter::equal_to(const Position& a, const Position& b) const {
    return (a.node_id() == b.node_id() &&
            a.is_reverse() == b.is_reverse() &&
            a.offset() == b.offset());
}

bool GAMSorter::less_than(const Position& a, const Position& b) const {
    if (a.node_id() < b.node_id()) {
        return true;
    } else if (a.node_id() > b.node_id()) {
        return false;
    }
    
    if (a.is_reverse() < b.is_reverse()) {
        return true;
    } else if (a.is_reverse() > b.is_reverse()) {
        return false;
    }

    if (a.offset() < b.offset()) {
        return true;
    }
    
    return false;
}

bool GAMSorter::greater_than(const Position& a, const Position& b) const {
    if (a.node_id() > b.node_id()) {
        return true;
    } else if (a.node_id() < b.node_id()) {
        return false;
    }
    
    if (a.is_reverse() > b.is_reverse()) {
        return true;
    } else if (a.is_reverse() < b.is_reverse()) {
        return false;
    }

    if (a.offset() > b.offset()) {
        return true;
    }
    
    return false;
}
