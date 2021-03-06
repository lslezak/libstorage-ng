/*
 * Copyright (c) [2004-2015] Novell, Inc.
 * Copyright (c) [2017-2019] SUSE LLC
 *
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, contact Novell, Inc.
 *
 * To contact Novell about this file by physical or electronic mail, you may
 * find current contact information at www.novell.com.
 */


#ifndef STORAGE_CMD_BTRFS_H
#define STORAGE_CMD_BTRFS_H


#include <string>
#include <vector>
#include <map>

#include "storage/Filesystems/BtrfsSubvolumeImpl.h"
#include "storage/Filesystems/Btrfs.h"


namespace storage
{
    using std::string;
    using std::vector;
    using std::map;


    /**
     * Class to probe for btrfs filesystems: Call "btrfs filesystem show"
     * (globally, not restricted to any disk or partition) and parse its
     * output.
     */
    class CmdBtrfsFilesystemShow
    {
    public:

	/**
	 * Constructor: Probe for btrfs filesystems with the
	 * "btrfs filesystem show" command and parse its output.
	 *
	 * This may throw a SystemCmdException or a ParseException.
	 */
	CmdBtrfsFilesystemShow();

	/**
	 * Device of a btrfs filesystem.
	 */
	struct Device
	{
	    Device() : id(0), name() {}

	    unsigned int id;
	    string name;
	};

	/**
	 * Entry for one btrfs filesystem. Since btrfs includes a volume
	 * manager (independent of LVM or the device mapper), this may be
	 * multiple devices for a single btrfs filesystem.
	 */
	struct Entry
	{
	    Entry() : uuid(), devices() {}

	    string uuid;
	    vector<Device> devices;
	};

	typedef vector<Entry>::value_type value_type;
	typedef vector<Entry>::const_iterator const_iterator;

	const_iterator begin() const { return data.begin(); }
	const_iterator end() const { return data.end(); }

	friend std::ostream& operator<<(std::ostream& s, const CmdBtrfsFilesystemShow& cmd_btrfs_filesystem_show);
	friend std::ostream& operator<<(std::ostream& s, const Entry& entry);
	friend std::ostream& operator<<(std::ostream& s, const Device& device);

    private:

	/**
	 * Parse the output of "btrfs filesystem show" passed in 'lines'.
	 *
	 * This may throw a ParseException.
	 */
	void parse(const vector<string>& lines);

	vector<Entry> data;

    };


    /**
     * Class to probe for btrfs subvolumes: Call "btrfs subvolume list
     * <mountpoint>".
     */
    class CmdBtrfsSubvolumeList
    {
    public:

	typedef string key_t;

	CmdBtrfsSubvolumeList(const key_t& key, const string& mount_point);

	struct Entry
	{
	    Entry()
		: id(BtrfsSubvolume::Impl::unknown_id),
		  parent_id(BtrfsSubvolume::Impl::unknown_id),
		  path() {}

	    long id;
	    long parent_id;
	    string path;
	};

	typedef vector<Entry>::value_type value_type;
	typedef vector<Entry>::const_iterator const_iterator;

	const_iterator begin() const { return data.begin(); }
	const_iterator end() const { return data.end(); }

	const_iterator find_entry_by_path(const string& path) const;

	friend std::ostream& operator<<(std::ostream& s, const CmdBtrfsSubvolumeList& cmd_btrfs_subvolume_list);
	friend std::ostream& operator<<(std::ostream& s, const Entry& entry);

    private:

	void parse(const vector<string>& lines);

	vector<Entry> data;

    };


    /**
     * Class to probe for btrfs default subvolume: Call "btrfs subvolume
     * get-default <mountpoint>".
     */
    class CmdBtrfsSubvolumeGetDefault
    {
    public:

	typedef string key_t;

	CmdBtrfsSubvolumeGetDefault(const key_t& key, const string& mount_point);

	long get_id() const { return id; }

	friend std::ostream& operator<<(std::ostream& s, const CmdBtrfsSubvolumeGetDefault&
					cmd_btrfs_subvolume_get_default);

    private:

	void parse(const vector<string>& lines);

	long id;

    };


    /**
     * Class to probe for btrfs RAID levels: Call "btrfs filesystem df
     * <mount-point>".
     */
    class CmdBtrfsFilesystemDf
    {
    public:

	typedef string key_t;

	CmdBtrfsFilesystemDf(const key_t& key, const string& mount_point);

	BtrfsRaidLevel get_metadata_raid_level() const { return metadata_raid_level; }
	BtrfsRaidLevel get_data_raid_level() const { return data_raid_level; }

	friend std::ostream& operator<<(std::ostream& s, const CmdBtrfsFilesystemDf&
					cmd_btrfs_filesystem_df);

    private:

	void parse(const vector<string>& lines);

	BtrfsRaidLevel metadata_raid_level;
	BtrfsRaidLevel data_raid_level;

    };

}

#endif
