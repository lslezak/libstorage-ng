/*
 * Copyright (c) [2014-2015] Novell, Inc.
 * Copyright (c) [2016-2017] SUSE LLC
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


#include <ctype.h>
#include <iostream>

#include "storage/Devices/MdImpl.h"
#include "storage/Devices/MdContainerImpl.h"
#include "storage/Devices/MdMemberImpl.h"
#include "storage/Holders/MdUserImpl.h"
#include "storage/Devicegraph.h"
#include "storage/Action.h"
#include "storage/Storage.h"
#include "storage/Prober.h"
#include "storage/Environment.h"
#include "storage/SystemInfo/SystemInfo.h"
#include "storage/Utils/AppUtil.h"
#include "storage/Utils/Exception.h"
#include "storage/Utils/Enum.h"
#include "storage/Utils/StorageTmpl.h"
#include "storage/Utils/StorageTypes.h"
#include "storage/Utils/StorageDefines.h"
#include "storage/Utils/SystemCmd.h"
#include "storage/Utils/StorageTmpl.h"
#include "storage/Utils/XmlFile.h"
#include "storage/Utils/HumanString.h"
#include "storage/Utils/Algorithm.h"
#include "storage/UsedFeatures.h"
#include "storage/EtcMdadm.h"


namespace storage
{

    using namespace std;


    const char* DeviceTraits<Md>::classname = "Md";


    // strings must match /proc/mdstat
    const vector<string> EnumTraits<MdLevel>::names({
	"unknown", "RAID0", "RAID1", "RAID5", "RAID6", "RAID10", "CONTAINER"
    });


    // strings must match "mdadm --parity" option
    const vector<string> EnumTraits<MdParity>::names({
	"default", "left-asymmetric", "left-symmetric", "right-asymmetric",
	"right-symmetric", "parity-first", "parity-last", "left-asymmetric-6",
	"left-symmetric-6", "right-asymmetric-6", "right-symmetric-6",
	"parity-first-6", "n2", "o2", "f2", "n3", "o3", "f3"
    });


    // Matches names of the form /dev/md<number> and /dev/md/<number>. The
    // latter looks like a named MD but since mdadm creates /dev/md<number> in
    // that case and not /dev/md<some big number> the number must be
    // considered in find_free_numeric_name().

    const regex Md::Impl::numeric_name_regex(DEVDIR "/md/?([0-9]+)", regex_constants::extended);


    // mdadm(8) states that any string for the names is allowed. That is
    // not correct: A '/' is reported as invalid by mdadm itself. A ' '
    // does not work, e.g. the links in /dev/md/ are broken.

    const regex Md::Impl::format1_name_regex(DEVMDDIR "/([^/ ]+)", regex_constants::extended);
    const regex Md::Impl::format2_name_regex(DEVMDDIR "_([^/ ]+)", regex_constants::extended);


    Md::Impl::Impl(const string& name)
	: Partitionable::Impl(name), md_level(MdLevel::UNKNOWN), md_parity(MdParity::DEFAULT),
	  chunk_size(0), uuid(), metadata(), in_etc_mdadm(true)
    {
	if (!is_valid_name(name))
	    ST_THROW(Exception("invalid Md name"));

	if (is_numeric())
	{
	    string::size_type pos = string(DEVDIR).size() + 1;
	    set_sysfs_name(name.substr(pos));
	    set_sysfs_path("/devices/virtual/block/" + name.substr(pos));
	}
    }


    Md::Impl::Impl(const xmlNode* node)
	: Partitionable::Impl(node), md_level(MdLevel::UNKNOWN), md_parity(MdParity::DEFAULT),
	  chunk_size(0), uuid(), metadata(), in_etc_mdadm(true)
    {
	string tmp;

	if (getChildValue(node, "md-level", tmp))
	    md_level = toValueWithFallback(tmp, MdLevel::RAID0);

	if (getChildValue(node, "md-parity", tmp))
	    md_parity = toValueWithFallback(tmp, MdParity::DEFAULT);

	getChildValue(node, "chunk-size", chunk_size);

	getChildValue(node, "uuid", uuid);

	getChildValue(node, "metadata", metadata);

	getChildValue(node, "in-etc-mdadm", in_etc_mdadm);
    }


    string
    Md::Impl::find_free_numeric_name(const Devicegraph* devicegraph)
    {
	vector<const Md*> mds = get_all_if(devicegraph, [](const Md* md) { return md->is_numeric(); });

	unsigned int free_number = first_missing_number(mds, 0);

	return DEVDIR "/md" + to_string(free_number);
    }


    void
    Md::Impl::check(const CheckCallbacks* check_callbacks) const
    {
	Partitionable::Impl::check(check_callbacks);

	if (get_region().get_start() != 0)
	    cerr << "md region start not zero" << endl;

	if (!is_valid_name(get_name()))
	    ST_THROW(Exception("invalid name"));
    }


    void
    Md::Impl::set_md_level(MdLevel md_level)
    {
	if (Impl::md_level == md_level)
	    return;

	Impl::md_level = md_level;

	calculate_region_and_topology();
    }


    void
    Md::Impl::set_chunk_size(unsigned long chunk_size)
    {
	if (Impl::chunk_size == chunk_size)
	    return;

	Impl::chunk_size = chunk_size;

	calculate_region_and_topology();
    }


    unsigned long
    Md::Impl::get_default_chunk_size() const
    {
	return 512 * KiB;
    }


    bool
    Md::Impl::is_valid_name(const string& name)
    {
	return regex_match(name, numeric_name_regex) || regex_match(name, format1_name_regex);
    }



    bool
    Md::Impl::is_valid_sysfs_name(const string& name)
    {
	return regex_match(name, numeric_name_regex) || regex_match(name, format2_name_regex);
    }


    bool
    Md::Impl::activate_mds(const ActivateCallbacks* activate_callbacks, const TmpDir& tmp_dir)
    {
	y2mil("activate_mds");

	// When using 'mdadm --assemble --scan' without the previously
	// generated config file some devices, e.g. members of IMSM
	// containers, get non 'local' names (ending in '_' followed by a
	// digit string). Using 'mdadm --assemble --scan --config=partitions'
	// the members of containers are not started at all.

	string filename = tmp_dir.get_fullname() + "/mdadm.conf";

	string cmd_line1 = MDADMBIN " --examine --scan > " + quote(filename);
	cout << cmd_line1 << endl;

	SystemCmd cmd1(cmd_line1);

	string cmd_line2 = MDADMBIN " --assemble --scan --config=" + quote(filename);
	cout << cmd_line2 << endl;

	SystemCmd cmd2(cmd_line2);

	if (cmd2.retcode() == 0)
	    SystemCmd(UDEVADMBIN_SETTLE);

	unlink(filename.c_str());

	return cmd2.retcode() == 0;
    }


    bool
    Md::Impl::deactivate_mds()
    {
	y2mil("deactivate_mds");

	string cmd_line = MDADMBIN " --stop --scan";
	cout << cmd_line << endl;

	SystemCmd cmd(cmd_line);

	return cmd.retcode() == 0;
    }


    void
    Md::Impl::probe_mds(Prober& prober)
    {
	for (const string& short_name : prober.get_system_info().getDir(SYSFSDIR "/block"))
	{
	    string name = DEVDIR "/" + short_name;
	    if (!is_valid_sysfs_name(name))
		continue;

	    // workaround for https://bugzilla.suse.com/show_bug.cgi?id=1030896
	    const ProcMdstat& proc_mdstat = prober.get_system_info().getProcMdstat();
	    if (!proc_mdstat.has_entry(short_name))
		continue;

	    const MdadmDetail& mdadm_detail = prober.get_system_info().getMdadmDetail(name);
	    if (!mdadm_detail.devname.empty())
		name = DEVMDDIR "/" + mdadm_detail.devname;

	    const ProcMdstat::Entry& entry = prober.get_system_info().getProcMdstat().get_entry(short_name);

	    if (entry.is_container)
	    {
		MdContainer* md_container = MdContainer::create(prober.get_probed(), name);
		md_container->get_impl().probe_pass_1a(prober);
	    }
	    else if (entry.has_container)
	    {
		MdMember* md_member = MdMember::create(prober.get_probed(), name);
		md_member->get_impl().probe_pass_1a(prober);
	    }
	    else
	    {
		Md* md = Md::create(prober.get_probed(), name);
		md->get_impl().probe_pass_1a(prober);
	    }
	}
    }


    void
    Md::Impl::probe_pass_1a(Prober& prober)
    {
	Partitionable::Impl::probe_pass_1a(prober);

	const ProcMdstat::Entry& entry = prober.get_system_info().getProcMdstat().get_entry(get_sysfs_name());
	md_parity = entry.md_parity;
	chunk_size = entry.chunk_size;

	const MdadmDetail& mdadm_detail = prober.get_system_info().getMdadmDetail(get_name());
	uuid = mdadm_detail.uuid;
	metadata = mdadm_detail.metadata;
	md_level = mdadm_detail.level;

	const EtcMdadm& etc_mdadm = prober.get_system_info().getEtcMdadm();
	in_etc_mdadm = etc_mdadm.has_entry(uuid);
    }


    void
    Md::Impl::probe_pass_1b(Prober& prober)
    {
	const ProcMdstat::Entry& entry = prober.get_system_info().getProcMdstat().get_entry(get_sysfs_name());

	for (const ProcMdstat::Device& device : entry.devices)
	{
	    prober.add_holder(device.name, get_non_impl(), [&device](Devicegraph* probed, Device* a, Device* b) {
		MdUser* md_user = MdUser::create(probed, a, b);
		md_user->set_spare(device.spare);
		md_user->set_faulty(device.faulty);
	    });
	}
    }


    void
    Md::Impl::probe_uuid()
    {
	MdadmDetail mdadm_detail(get_name());
	uuid = mdadm_detail.uuid;
    }


    void
    Md::Impl::parent_has_new_region(const Device* parent)
    {
	calculate_region_and_topology();
    }


    void
    Md::Impl::add_create_actions(Actiongraph::Impl& actiongraph) const
    {
	vector<Action::Base*> actions;

	actions.push_back(new Action::Create(get_sid()));

	if (in_etc_mdadm)
	    actions.push_back(new Action::AddToEtcMdadm(get_sid()));

	actiongraph.add_chain(actions);

	// see Encryption::Impl::add_create_actions()

	if (in_etc_mdadm)
	{
	    actions[0]->last = true;
	    actions[1]->last = false;
	}
    }


    void
    Md::Impl::add_modify_actions(Actiongraph::Impl& actiongraph, const Device* lhs_base) const
    {
	BlkDevice::Impl::add_modify_actions(actiongraph, lhs_base);

	const Impl& lhs = dynamic_cast<const Impl&>(lhs_base->get_impl());

	if (lhs.get_name() != get_name())
	    ST_THROW(Exception("cannot rename raid"));

	if (lhs.md_level != md_level)
	    ST_THROW(Exception("cannot change raid level"));

	if (lhs.metadata != metadata)
	    ST_THROW(Exception("cannot change raid metadata"));

	if (lhs.chunk_size != chunk_size)
	    ST_THROW(Exception("cannot change chunk size"));

	if (lhs.get_region() != get_region())
	    ST_THROW(Exception("cannot change size"));

	if (!lhs.in_etc_mdadm && in_etc_mdadm)
	{
	    Action::Base* action = new Action::AddToEtcMdadm(get_sid());
	    actiongraph.add_vertex(action);
	}
	else if (lhs.in_etc_mdadm && !in_etc_mdadm)
	{
	    Action::Base* action = new Action::RemoveFromEtcMdadm(get_sid());
	    actiongraph.add_vertex(action);
	}
    }


    void
    Md::Impl::add_delete_actions(Actiongraph::Impl& actiongraph) const
    {
	vector<Action::Base*> actions;

	if (in_etc_mdadm)
	    actions.push_back(new Action::RemoveFromEtcMdadm(get_sid()));

	if (is_active())
	    actions.push_back(new Action::Deactivate(get_sid()));

	actions.push_back(new Action::Delete(get_sid()));

	actiongraph.add_chain(actions);
    }


    void
    Md::Impl::save(xmlNode* node) const
    {
	Partitionable::Impl::save(node);

	setChildValue(node, "md-level", toString(md_level));
	setChildValueIf(node, "md-parity", toString(md_parity), md_parity != MdParity::DEFAULT);

	setChildValueIf(node, "chunk-size", chunk_size, chunk_size != 0);

	setChildValueIf(node, "uuid", uuid, !uuid.empty());

	setChildValueIf(node, "metadata", metadata, !metadata.empty());

	setChildValueIf(node, "in-etc-mdadm", in_etc_mdadm, !in_etc_mdadm);
    }


    MdUser*
    Md::Impl::add_device(BlkDevice* blk_device)
    {
	if (blk_device->num_children() != 0)
	    ST_THROW(WrongNumberOfChildren(blk_device->num_children(), 0));

	MdUser* md_user = MdUser::create(get_devicegraph(), blk_device, get_non_impl());

	calculate_region_and_topology();

	return md_user;
    }


    void
    Md::Impl::remove_device(BlkDevice* blk_device)
    {
	MdUser* md_user = to_md_user(get_devicegraph()->find_holder(blk_device->get_sid(), get_sid()));

	get_devicegraph()->remove_holder(md_user);

	calculate_region_and_topology();
    }


    vector<BlkDevice*>
    Md::Impl::get_devices()
    {
	Devicegraph::Impl& devicegraph = get_devicegraph()->get_impl();
	Devicegraph::Impl::vertex_descriptor vertex = get_vertex();

	// TODO sorting

	return devicegraph.filter_devices_of_type<BlkDevice>(devicegraph.parents(vertex));
    }


    vector<const BlkDevice*>
    Md::Impl::get_devices() const
    {
	const Devicegraph::Impl& devicegraph = get_devicegraph()->get_impl();
	Devicegraph::Impl::vertex_descriptor vertex = get_vertex();

	// TODO sorting

	return devicegraph.filter_devices_of_type<const BlkDevice>(devicegraph.parents(vertex));
    }


    bool
    Md::Impl::is_numeric() const
    {
	return regex_match(get_name(), numeric_name_regex);
    }


    unsigned int
    Md::Impl::get_number() const
    {
	smatch match;

	if (!regex_match(get_name(), match, numeric_name_regex) || match.size() != 2)
	    ST_THROW(Exception("not a numeric Md"));

	return atoi(match[1].str().c_str());
    }


    uint64_t
    Md::Impl::used_features() const
    {
	return UF_MDRAID | Partitionable::Impl::used_features();
    }


    bool
    Md::Impl::equal(const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	if (!Partitionable::Impl::equal(rhs))
	    return false;

	return md_level == rhs.md_level && md_parity == rhs.md_parity &&
	    chunk_size == rhs.chunk_size && metadata == rhs.metadata &&
	    uuid == rhs.uuid && in_etc_mdadm == rhs.in_etc_mdadm;
    }


    void
    Md::Impl::log_diff(std::ostream& log, const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	Partitionable::Impl::log_diff(log, rhs);

	storage::log_diff_enum(log, "md-level", md_level, rhs.md_level);
	storage::log_diff_enum(log, "md-parity", md_parity, rhs.md_parity);

	storage::log_diff(log, "chunk-size", chunk_size, rhs.chunk_size);

	storage::log_diff(log, "metadata", metadata, rhs.metadata);

	storage::log_diff(log, "uuid", uuid, rhs.uuid);

	storage::log_diff(log, "in-etc-mdadm", in_etc_mdadm, rhs.in_etc_mdadm);
    }


    void
    Md::Impl::print(std::ostream& out) const
    {
	Partitionable::Impl::print(out);

	out << " md-level:" << toString(get_md_level());
	out << " md-parity:" << toString(get_md_parity());

	out << " chunk-size:" << get_chunk_size();

	out << " metadata:" << metadata;

	out << " uuid:" << uuid;

	out << " in-etc-mdadm:" << in_etc_mdadm;
    }


    void
    Md::Impl::process_udev_ids(vector<string>& udev_ids) const
    {
	// See doc/udev.md.

	erase_if(udev_ids, [](const string& udev_id) {
	    return !boost::starts_with(udev_id, "md-uuid-");
	});
    }


    unsigned int
    Md::Impl::minimal_number_of_devices() const
    {
	switch (md_level)
	{
	    case MdLevel::RAID0:
		return 2;

	    case MdLevel::RAID1:
		return 2;

	    case MdLevel::RAID5:
		return 3;

	    case MdLevel::RAID6:
		return 4;

	    case MdLevel::RAID10:
		return 2;

	    default:
		return 0;
	}
    }


    void
    Md::Impl::calculate_region_and_topology()
    {
	// Calculating the exact size of a MD is difficult. Since a size too
	// big can lead to severe problems later on, e.g. a partition not
	// fitting anymore, we make a conservative calculation.

	const bool conservative = true;

	// Since our size calculation is not accurate we must not recalculate
	// the size of an RAID existing on disk. That would cause a resize
	// action to be generated. Operations changing the RAID size are not
	// supported.

	if (exists_in_probed())
	    return;

	vector<BlkDevice*> devices = get_devices();

	long real_chunk_size = chunk_size;

	if (real_chunk_size == 0)
	    real_chunk_size = get_default_chunk_size();

	// mdadm uses a chunk size of 64 KiB just in case the RAID1 is ever reshaped to RAID5.
	if (md_level == MdLevel::RAID1)
	    real_chunk_size = 64 * KiB;

	int number = 0;
	unsigned long long sum = 0;
	unsigned long long smallest = std::numeric_limits<unsigned long long>::max();

	for (const BlkDevice* blk_device : devices)
	{
	    unsigned long long size = blk_device->get_size();

	    const MdUser* md_user = blk_device->get_impl().get_single_out_holder_of_type<const MdUser>();
	    bool spare = md_user->is_spare();

	    // metadata for version 1.0 is 4 KiB block at end aligned to 4 KiB,
	    // https://raid.wiki.kernel.org/index.php/RAID_superblock_formats
	    size = (size & ~(0x1000ULL - 1)) - 0x2000;

	    // size used for bitmap depends on device size

	    if (conservative)
	    {
		// trim device size by 128 MiB but not more than roughly 1%
		size -= min(128 * MiB, size / 64);
	    }

	    long rest = size % real_chunk_size;
	    if (rest > 0)
		size -= rest;

	    if (!spare)
	    {
		number++;
		sum += size;
	    }

	    smallest = min(smallest, size);
	}

	unsigned long long size = 0;
	long optimal_io_size = 0;

	switch (md_level)
	{
	    case MdLevel::RAID0:
		if (number >= 2)
		{
		    size = sum;
		    optimal_io_size = real_chunk_size * number;
		}
		break;

	    case MdLevel::RAID1:
		if (number >= 2)
		{
		    size = smallest;
		    optimal_io_size = 0;
		}
		break;

	    case MdLevel::RAID5:
		if (number >= 3)
		{
		    size = smallest * (number - 1);
		    optimal_io_size = real_chunk_size * (number - 1);
		}
		break;

	    case MdLevel::RAID6:
		if (number >= 4)
		{
		    size = smallest * (number - 2);
		    optimal_io_size = real_chunk_size * (number - 2);
		}
		break;

	    case MdLevel::RAID10:
		if (number >= 2)
		{
		    size = ((smallest / real_chunk_size) * number / 2) * real_chunk_size;
		    optimal_io_size = real_chunk_size * number / 2;
		    if (number % 2 == 1)
			optimal_io_size *= 2;
		}
		break;

	    case MdLevel::CONTAINER:
	    case MdLevel::UNKNOWN:
		break;
	}

	set_size(size);
	set_topology(Topology(0, optimal_io_size));
    }


    Text
    Md::Impl::do_create_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Create MD %1$s %2$s (%3$s)"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Creating MD %1$s %2$s (%3$s)"));

	return sformat(text, get_md_level_name(md_level).c_str(), get_displayname().c_str(),
		       get_size_string().c_str());
    }


    void
    Md::Impl::do_create()
    {
	// Note: Changing any parameter to "mdadm --create' requires the
	// function calculate_region_and_topology() to be checked!

	string cmd_line = MDADMBIN " --create " + quote(get_name()) + " --run --level=" +
	    boost::to_lower_copy(toString(md_level), locale::classic()) + " --metadata=1.0"
	    " --homehost=any";

	if (md_level == MdLevel::RAID1 || md_level == MdLevel::RAID5 ||
	    md_level == MdLevel::RAID6 || md_level == MdLevel::RAID10)
	    cmd_line += " --bitmap=internal";

	if (chunk_size > 0)
	    cmd_line += " --chunk=" + to_string(chunk_size / KiB);

	if (md_parity != MdParity::DEFAULT)
	    cmd_line += " --parity=" + toString(md_parity);

	// place devices in multimaps to sort them according to the sort-key

	multimap<unsigned int, string> devices;
	multimap<unsigned int, string> spares;

	for (const BlkDevice* blk_device : get_devices())
	{
	    const MdUser* md_user = blk_device->get_impl().get_single_out_holder_of_type<const MdUser>();

	    if (!md_user->is_spare())
		devices.insert(make_pair(md_user->get_sort_key(), blk_device->get_name()));
	    else
		spares.insert(make_pair(md_user->get_sort_key(), blk_device->get_name()));
	}

	cmd_line += " --raid-devices=" + to_string(devices.size());

	if (!spares.empty())
	    cmd_line += " --spare-devices=" + to_string(spares.size());

	for (const pair<unsigned int, string>& value : devices)
	    cmd_line += " " + quote(value.second);

	for (const pair<unsigned int, string>& value : spares)
	    cmd_line += " " + quote(value.second);

	cout << cmd_line << endl;

	SystemCmd cmd(cmd_line);
	if (cmd.retcode() != 0)
	    ST_THROW(Exception("create md raid failed"));

	probe_uuid();
    }


    Text
    Md::Impl::do_delete_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Delete MD %1$s %2$s (%3$s)"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Deleting MD %1$s %2$s (%3$s)"));

	return sformat(text, get_md_level_name(md_level).c_str(), get_displayname().c_str(),
		       get_size_string().c_str());
    }


    void
    Md::Impl::do_delete() const
    {
	for (const BlkDevice* blk_device : get_devices())
	{
	    blk_device->get_impl().wipe_device();
	}
    }


    Text
    Md::Impl::do_add_to_etc_mdadm_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by md name (e.g. /dev/md0)
			   _("Add %1$s to /etc/mdadm.conf"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by md name (e.g. /dev/md0)
			   _("Adding %1$s to /etc/mdadm.conf"));

	return sformat(text, get_name().c_str());
    }


    void
    Md::Impl::do_add_to_etc_mdadm(CommitData& commit_data) const
    {
	EtcMdadm& etc_mdadm = commit_data.get_etc_mdadm();

	etc_mdadm.init(get_storage());

	EtcMdadm::Entry entry;

	entry.device = get_name();
	entry.uuid = uuid;

	etc_mdadm.update_entry(entry);
    }


    Text
    Md::Impl::do_remove_from_etc_mdadm_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by md name (e.g. /dev/md0)
			   _("Remove %1$s from /etc/mdadm.conf"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by md name (e.g. /dev/md0)
			   _("Removing %1$s from /etc/mdadm.conf"));

	return sformat(text, get_name().c_str());
    }


    void
    Md::Impl::do_remove_from_etc_mdadm(CommitData& commit_data) const
    {
	EtcMdadm& etc_mdadm = commit_data.get_etc_mdadm();

	// TODO containers?

	etc_mdadm.remove_entry(uuid);
    }


    Text
    Md::Impl::do_reallot_text(ReallotMode reallot_mode, const Device* device, Tense tense) const
    {
	Text text;

	switch (reallot_mode)
	{
	    case ReallotMode::REDUCE:
		text = tenser(tense,
			      // TRANSLATORS: displayed before action,
			      // %1$s is replaced by device name (e.g. /dev/sdd),
			      // %2$s is replaced by device name (e.g. /dev/md0)
			      _("Remove %1$s from %2$s"),
			      // TRANSLATORS: displayed during action,
			      // %1$s is replaced by device name (e.g. /dev/sdd),
			      // %2$s is replaced by device name (e.g. /dev/md0)
			      _("Removing %1$s from %2$s"));
		break;

	    case ReallotMode::EXTEND:
		text = tenser(tense,
			      // TRANSLATORS: displayed before action,
			      // %1$s is replaced by device name (e.g. /dev/sdd),
			      // %2$s is replaced by device name (e.g. /dev/md0)
			      _("Add %1$s to %2$s"),
			      // TRANSLATORS: displayed during action,
			      // %1$s is replaced by device name (e.g. /dev/sdd),
			      // %2$s is replaced by device name (e.g. /dev/md0)
			      _("Adding %1$s to %2$s"));
		break;

	    default:
		ST_THROW(LogicException("invalid value for reallot_mode"));
	}

	return sformat(text, to_blk_device(device)->get_name().c_str(), get_displayname().c_str());
    }


    void
    Md::Impl::do_reallot(ReallotMode reallot_mode, const Device* device) const
    {
	const BlkDevice* blk_device = to_blk_device(device);

	switch (reallot_mode)
	{
	    case ReallotMode::REDUCE:
		do_reduce(blk_device);
		return;

	    case ReallotMode::EXTEND:
		do_extend(blk_device);
		return;
	}

	ST_THROW(LogicException("invalid value for reallot_mode"));
    }


    void
    Md::Impl::do_reduce(const BlkDevice* blk_device) const
    {
	string cmd_line = MDADMBIN " --remove " + quote(get_name()) + " " + quote(blk_device->get_name());
	cout << cmd_line << endl;

	SystemCmd cmd(cmd_line);
	if (cmd.retcode() != 0)
	    ST_THROW(Exception("reduce md failed"));

	// Thanks to udev "md-raid-assembly.rules" running "parted <disk>
	// print" readds the device to the md if the signature is still
	// valid. Thus remove the signature.
	blk_device->get_impl().wipe_device();
    }


    void
    Md::Impl::do_extend(const BlkDevice* blk_device) const
    {
	const MdUser* md_user = blk_device->get_impl().get_single_out_holder_of_type<const MdUser>();

	string cmd_line = MDADMBIN;
	cmd_line += !md_user->is_spare() ? " --add" : " --add-spare";
	cmd_line += " " + quote(get_name()) + " " + quote(blk_device->get_name());
	cout << cmd_line << endl;

	SystemCmd cmd(cmd_line);
	if (cmd.retcode() != 0)
	    ST_THROW(Exception("extend md failed"));
    }


    Text
    Md::Impl::do_deactivate_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2 GiB)
			   _("Deactivate MD %1$s %2$s (%3$s)"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2 GiB)
			   _("Deactivating MD %1$s %2$s (%3$s)"));

	return sformat(text, get_md_level_name(md_level).c_str(), get_displayname().c_str(),
		       get_size_string().c_str());
    }


    void
    Md::Impl::do_deactivate() const
    {
	string cmd_line = MDADMBIN " --stop " + quote(get_name());
	cout << cmd_line << endl;

	SystemCmd cmd(cmd_line);
	if (cmd.retcode() != 0)
	    ST_THROW(Exception("deactivate md raid failed"));
    }


    namespace Action
    {

	Text
	AddToEtcMdadm::text(const CommitData& commit_data) const
	{
	    const Md* md = to_md(get_device(commit_data.actiongraph, RHS));
	    return md->get_impl().do_add_to_etc_mdadm_text(commit_data.tense);
	}


	void
	AddToEtcMdadm::commit(CommitData& commit_data, const CommitOptions& commit_options) const
	{
	    const Md* md = to_md(get_device(commit_data.actiongraph, RHS));
	    md->get_impl().do_add_to_etc_mdadm(commit_data);
	}


	void
	AddToEtcMdadm::add_dependencies(Actiongraph::Impl::vertex_descriptor vertex,
					Actiongraph::Impl& actiongraph) const
	{
	    Modify::add_dependencies(vertex, actiongraph);

	    if (actiongraph.mount_root_filesystem != actiongraph.vertices().end())
		actiongraph.add_edge(*actiongraph.mount_root_filesystem, vertex);
	}


	Text
	RemoveFromEtcMdadm::text(const CommitData& commit_data) const
	{
	    const Md* md = to_md(get_device(commit_data.actiongraph, LHS));
	    return md->get_impl().do_remove_from_etc_mdadm_text(commit_data.tense);
	}


	void
	RemoveFromEtcMdadm::commit(CommitData& commit_data, const CommitOptions& commit_options) const
	{
	    const Md* md = to_md(get_device(commit_data.actiongraph, LHS));
	    md->get_impl().do_remove_from_etc_mdadm(commit_data);
	}

    }


    bool
    compare_by_name_and_number(const Md* lhs, const Md* rhs)
    {
	bool numeric_lhs = lhs->is_numeric();
	bool numeric_rhs = rhs->is_numeric();

	if (!numeric_lhs && !numeric_rhs)
	    return lhs->get_name() < rhs->get_name();
	else if (numeric_lhs && numeric_rhs)
	    return lhs->get_number() < rhs->get_number();
	else
	    return numeric_lhs < numeric_rhs;
    }

}
