#!/usr/bin/perl
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
# Or, point your browser to http://www.gnu.org/copyleft/gpl.html
#
# Parts of this code are taken from VDRAdmin-AM 
# (C) 2005 - 2008 by Andreas Mair <mail@andreas.vdr-developer.org>



use Socket;
use POSIX;

my (%CONFIG);
$CONFIG{VDR_HOST} = "localhost";	# Name or IP address of VDR server
$CONFIG{VDR_PORT} = 6419;		# SVDRP port on VDR server (use 2001 pre - 1.7.15)
$CONFIG{SERIES_TIMEOUT} = 30;		# Expiry time of a series (days)
$CONFIG{START_PADDING} = 1;		# Padding when creating new timers without VPS
$CONFIG{STOP_PADDING} = 3;
$CONFIG{PRIORITY} = 99;			# Recording priority and lifetime
$CONFIG{LIFETIME} = 99;
$CONFIG{LINKSDIR} = "/video/video";	# Directory holding links file
$CONFIG{VPS} = 1;			# 1 = control timer from EIT running status

if ($CONFIG{VPS}) {
  $CONFIG{START_PADDING} = 0;
  $CONFIG{VPS} = 1;
}

my (@timers, @chans, @epg);
my %links = {};
my $updates = 0;

open_SVDRP();
get_channels();
get_epg();
get_timers();
get_links();
if (scalar (@timers)) {
  $updates = check_timers();
}
$updates += check_links();
if (scalar (@timers)) {
  $updates += check_split_recordings();
  if ($updates) {
    put_links();
    @timers = ();
    get_timers();
  }
#show_links();
  check_timer_clashes();
  check_changed_events();
}
close_SVDRP();

# Examine each timer and update the links file if necessary (manually-added timers)

sub check_timers {

  my $count = 0;
  foreach my $timer (@timers) {
    my $channelid = $chans[$timer->{chan}] -> {id};
    my $start_t = $timer->{tstart};
    my $stop_t = $timer->{tstop};
    foreach my $prog (@epg) {
      my $scrid = $prog->{scrid};
      my $icrid = $prog->{icrid};
      next if $scrid eq 'NULL';
      if (($prog->{sid} eq $channelid) && ($start_t <= $prog->{st}) && ($stop_t >= $prog->{et})) {
        if (exists $links{$scrid}) {			# Existing series
          if ($links{$scrid} !~ /$icrid/) {
	    my ($last_t,$val) = split(';', $links{$scrid});
            $links{$scrid} = "$start_t;$val:$icrid";    # New episode already added
	    $count++;
          }
        }
        else {					# New series added to timer
          $links{$scrid} = "$start_t;$icrid";
          $count++;
        }
      }
    }
  }
  return $count;
}

# Check for new EPG entries for each series in the links file and create timers.
# FIXME This algorithm fails if an item is part of two series and both are being
# recorded (how can that happen?).

sub check_links {
  my $count = 0;
  foreach my $prog (@epg) {
    my $scrid = $prog->{scrid};
    my $icrid = $prog->{icrid};
    next if $scrid eq 'NULL';
    if (exists $links{$scrid}) {
      if ($links{$scrid} !~ /$icrid/) {
#		Have we already recorded this programme on a diferent series?
	my $title = get_title($prog->{sid},$prog->{st});
	my $done = 0;
	for (values %links) {
	  if (/$icrid/) {
            print STDOUT "Item $icrid ($title) already recorded!\n";
	    $done = 1;
	  }
	}
	if (!$done) {
	  my $fstart = strftime("%Y-%m-%d:%H%M", localtime($prog->{st}-$CONFIG{START_PADDING}*60));
	  my $fend = strftime("%H%M", localtime($prog->{et}+$CONFIG{STOP_PADDING}*60));
	  my $flag = 1 + 4*$CONFIG{VPS};
          print STDOUT "New timer set for $scrid, \"$title\" at $fstart\n";
	  set_timer ("$flag:$prog->{sid}:$fstart:$fend:$CONFIG{PRIORITY}:$CONFIG{LIFETIME}:$title:");
	  my ($last_t,$val) = split(';', $links{$scrid});
          $links{$scrid} = "$prog->{st};$val:$icrid";
	  $count++;
	}
      }
    }
  }
  return $count;
}

# Review the timer list for clashes. FIXME: What happens when there are multiple cards?

sub check_timer_clashes {
  my ($ii, $jj);
  my (@tstart, @tstop);
  print STDOUT "Checking for timer clashes\n";
  for ($ii = 0 ; $ii < @timers ; $ii++) {
    push @tstart, $timers[$ii]->{tstart};
    push @tstop, $timers[$ii]->{tstop};

    for ($jj = 0 ; $jj < $ii ; $jj++) {
      if (is_clash($ii, $jj)) {
 # What to do?? For now just report the collision
        my $start1 = $tstart[$ii]+$CONFIG{START_PADDING}*60;
        my $start2 = $tstart[$jj]+$CONFIG{START_PADDING}*60;
        my $ttl1 = get_title($chans[$timers[$ii]->{chan}]->{id}, $start1);
        my $ttl2 = get_title($chans[$timers[$jj]->{chan}]->{id}, $start2);
        my $crid1 = get_crid_for_timer($chans[$timers[$ii]->{chan}]->{id}, $start1);
        my $crid2 = get_crid_for_timer($chans[$timers[$jj]->{chan}]->{id}, $start2);
        my $when = localtime ($timers[$ii]->{tstart});
        print STDOUT "Collision! $when\n$ttl1 ($crid1) <-> $ttl2 ($crid2)\n";
        list_alternatives($crid1, $ttl1, $start1);
        list_alternatives($crid2, $ttl2, $start2);
      }
    }
  }

  sub is_clash {
    my ($i, $j) = @_;
    if (($tstart[$i] >= $tstart[$j] && $tstart[$i] < $tstop[$j])
        || ($tstart[$j] >= $tstart[$i] && $tstart[$j] < $tstop[$i])) {
                # Timers collide in time. Check if the
		# Timers are on the same transponder
       my $t1 = $chans[$timers[$i]->{chan}] -> {transponder};
       my $t2 = $chans[$timers[$j]->{chan}] -> {transponder};
       if ($t1 eq $t2) {
#              print STDOUT "Multiple recordings on same transponder - OK\n";
         return 0;
       }
       else {
         return 1;
       }
    }
    return 0;
  }

  sub list_alternatives {
    my ($crid, $ttl, $start) = @_;

    if (my @prog = events_for_crid($crid)) {
      print STDOUT "Alternatives for $ttl:\n";
      foreach my $evt (@prog) {
        if ($evt->{st} != $start) {
          my $w = localtime($evt->{st});
          my ($c) = channel_name_from_id($evt->{sid}) =~ /(.*?);/;
          print STDOUT  "\t$c - $w\n";
        }
      }
    }
  }
}

# Look for split events (eg a film with a news summary in the middle), and ensure
# that all parts of the event are being recorded.
# A split event has a # in the item CRID, for example /19778232#1
# FIXME this will likely fail if a split event is repeated or is part of a series.

sub check_split_recordings {

  my $count = 0;
  my @splits;
  print STDOUT "Checking for split recordings\n";
  foreach my $timer (@timers) {
    my $channelid = $chans[$timer->{chan}] -> {id};
    my $start_t = $timer->{tstart};
    my $stop_t = $timer->{tstop};
    foreach my $prog (@epg) {
      if (($prog->{sid} eq $channelid) && ($start_t <= $prog->{st}) && ($stop_t >= $prog->{et})) {
        if ($prog->{icrid} =~ /\#/) {
          push @splits, "$prog->{icrid}:$prog->{st}:$channelid";
        }
      }
    }
  }
  foreach $split (@splits) {
    my ($tcrid, $tstart, $tchan) = split(':', $split);
    foreach my $prog (@epg) {
      if (($prog->{icrid} eq $tcrid) && ($tchan eq $prog->{sid}) && ($prog->{st} != $tstart)) {
        my $got = 0;
        foreach $split2 (@splits) {
          my ($tcrid2, $tstart2, $tchan2) = split(':', $split2);
          if (($prog->{icrid} eq $tcrid2) && ($tchan2 eq $prog->{sid}) && ($prog->{st} == $tstart2)) {
            $got = 1;
          }
        }
        if (!$got) {
          print STDOUT "Unset timer for split event $tcrid found!\n";
          my $fstart = strftime("%Y-%m-%d:%H%M", localtime($prog->{st}-$CONFIG{START_PADDING}*60));
          my $fend = strftime("%H%M", localtime($prog->{et}+$CONFIG{STOP_PADDING}*60));
          my $title = get_title($prog->{sid},$prog->{st});
          my $flag = 1 + 4*$CONFIG{VPS};
          print STDOUT "New timer set for \"$title\" at $fstart\n";
          set_timer ("$flag:$prog->{sid}:$fstart:$fend:$CONFIG{PRIORITY}:$CONFIG{LIFETIME}:$title:");
          $count++;
        }
      }
    }
  }
  return $count;
}

#	Scan the timers list for events which have changed since the timer was
#	set. For now we just report the error. TODO: if the event has moved maybe 
#	we should try to find the new one?

sub check_changed_events {

  print STDOUT "Checking for changed timer events\n";
  foreach my $timer (@timers) {
    my $channelid = $chans[$timer->{chan}] -> {id};
    my $start_t = $timer->{tstart};
    my $timer_title = $timer ->{title};
    my $event_title = get_title ($channelid, $start_t);
    if ($timer_title ne $event_title) {
      print STDOUT "Event: $event_title <=> Timer: $timer_title\n";
    }
  }
}

# Get a list of all occurrences of an event CRID. Return an array of EPG objects.

sub events_for_crid {

  my ($crid) = @_;
  my @elist;

  foreach my $prog (@epg) {
    if ($prog->{icrid} eq $crid) {
      push (@elist, $prog);
    }
  }
  return @elist;
}

# Get the name of a channel given its ID.

sub channel_name_from_id {

  my ($id) = @_;
  foreach my $chan (@chans) {
    if ($chan->{id} eq $id) {
      return $chan->{name};
    }
  }
  return ('Unknown');
}

# Read the timers from VDR

sub get_timers {

  Send("LSTT");
  while (<SOCK>) {
    chomp;
    if (/^\d*([- ])\d* (.*)/) {
      my ($flag,$chan,$day,$start,$stop,$prio,$life,$title) = split(':', $2);
      my ($yy,$mm,$dd) = split('-', $day);
      my $starth = $start / 100;
      my $startm = $start % 100;
      my $stoph = $stop / 100;
      my $stopm = $stop % 100;
      my $tstart = mktime(0, $startm, $starth, $dd, $mm-1, $yy-1900, 0, 0, -1);
      if ($stoph < $starth) {	# prog over midnight
        $dd++;
      }
      my $tstop = mktime(0, $stopm, $stoph, $dd, $mm-1, $yy-1900, 0, 0, -1);
      push (@timers, {
	flag => $flag,
	chan => $chan,
	tstart => $tstart,
	tstop => $tstop,
	title => $title
      });
    }
    last if $1 ne "-";
  }
  print STDOUT "Read ",scalar(@timers)," Timers\n";
}

# Read the EPG from VDR (TVAnytime events only)

sub get_epg {

  my ($sid,$id,$st,$et,$d);
  my $icrid = "NULL";
  my $scrid = "NULL";
  Send("LSTE");
  while (<SOCK>) {
    chomp;
    if (my ($flag,$type,$data) = /^215(.)(.) *(.*)$/) {
    if ($type eq 'C') {
      ($sid) = ($data =~ /^(.*?) /);
    }
    elsif ($type eq 'Y') {
      $icrid = $data;
    }
    elsif ($type eq 'Z') {
      $scrid = $data;
    }
    elsif ($type eq 'E') {
      ($id,$st,$d) = split(" ",$data);
      $et = $st + $d;
    }
    elsif ($type eq 'e') {
      if ($icrid ne "NULL") {
        push (@epg, {
          sid => $sid,
          st => $st,
          et => $et,
          id => $id,
          icrid => $icrid,
          scrid => $scrid
        });
        $icrid = "NULL";
      }
      $scrid = "NULL";
    }
    last if $flag ne "-";
}
  }
  print STDOUT "Read ",scalar(@epg)," EPG lines\n";
}

# Read the channels list from VDR. Channel numbers are 1-based not 0-based...

sub get_channels {

  push (@chans, {
    id => '0-0-0-0',
    transponder => '0-0',
    name => 'Dummy'
  });
  Send("LSTC");
  while (<SOCK>) {
    chomp;
    if (/^\d*([- ])\d* (.*)/) {
      my ($name,$f,$p,$t,$d4,$d5,$d6,$d7,$d8,$id1,$id2,$id3) = split(':', $2);
      push (@chans, {
	id => join('-', $t, $id2, $id3, $id1),
	transponder => join('-', $t, $f),
	name => $name
      });
    }
    last if $1 ne "-";
  }
  print STDOUT "Read ",scalar(@chans)," Channels\n";
}

# Read the links file.

sub get_links {

  if (open (LINKS,'<',"$CONFIG{LINKSDIR}/links.data")) {
    while (<LINKS>) {
      chomp;
      my ($scrid,$icrids) = split(',');
      $links{$scrid} = $icrids;
    }
    close (LINKS);
    print STDOUT "Read ",scalar(keys(%links))," Links\n";
  }
  else {
    print STDOUT "No links file found\n";
  }
}

# Save the links file

sub put_links {

  print STDOUT "Rewriting Links file\n";
  open (LINKS,'>',"$CONFIG{LINKSDIR}/links.data.new") or die "Cannot open new links file\n";
  while (my($link,$val) = each %links){
    if ($val ne '') {
      my ($last_t,$entries) = split(';',$val);
      if (($last_t + ($CONFIG{SERIES_TIMEOUT} * 86640)) > time()) {
        print LINKS $link,',',$val,"\n";
      }
      else {
	print STDOUT "Expiring series $link\n";
      }
    }
  }
  close (LINKS);
  if (-e "$CONFIG{LINKSDIR}/links.data.old") {
    unlink "$CONFIG{LINKSDIR}/links.data.old";
  }
  if (-e "$CONFIG{LINKSDIR}/links.data") {
    rename "$CONFIG{LINKSDIR}/links.data", "$CONFIG{LINKSDIR}/links.data.old";
  }
  rename "$CONFIG{LINKSDIR}/links.data.new", "$CONFIG{LINKSDIR}/links.data";
  print STDOUT "Wrote ",scalar(keys(%links))," Links\n";
}

# Display the links

sub show_links {

  while (my($link,$val) = each %links){
    if ($val ne '') {
      my ($last_t,$entries) = split(';',$val);
      my $last = strftime("%Y-%m-%d:%H%M", localtime($last_t));
      print STDOUT "$link\t$last\n";
    }
  }
}

# Get the program title from EPG, given channel and start time.

sub get_title {

  my $title = "TITLE";
  my ($chan,$time) = @_;
  Send ("LSTE $chan at $time");
  while (<SOCK>) {
    chomp;
    if (/^215-T (.*)/) {
      $title = $1;
    }
    last if substr($_, 3, 1) ne "-";
  }
  $title =~ s/:/|/g;
  return ($title);
}

# Get the item CRID for a timer entry.

sub get_crid_for_timer {

  my $crid = "CRID";
  my ($chan,$time) = @_;
  Send ("LSTE $chan at $time");
  while (<SOCK>) {
    chomp;
    if (/^215-Y (.*)/) {
      $crid = $1;
    }
    last if substr($_, 3, 1) ne "-";
  }
  $crid =~ s/:/|/g;
  return ($crid);
}


# Set a new timer

sub set_timer {

  my $string = shift;
  Send ("NEWT $string");
  Receive();
}

# SVDRP handling

sub open_SVDRP {

  $SIG{ALRM} = sub { Error("timeout"); };
  alarm($Timeout);

  $iaddr = inet_aton($CONFIG{VDR_HOST})       || Error("no host: $Dest");
  $paddr = sockaddr_in($CONFIG{VDR_PORT}, $iaddr);

  $proto = getprotobyname('tcp');
  socket(SOCK, PF_INET, SOCK_STREAM, $proto)  || Error("socket: $!");
  connect(SOCK, $paddr)                       || Error("connect: $!");
  select(SOCK); $| = 1;
  Receive();
}

sub close_SVDRP {
  print STDOUT "Closing connection\n";
  Send("quit");
  Receive();
  close(SOCK)                                 || Error("close: $!");
}

sub Send
{
  my $cmd = shift || Error("no command to send");
  print SOCK "$cmd\r\n";
}

sub Receive
{
  while (<SOCK>) {
#        print STDOUT $_;
        last if substr($_, 3, 1) ne "-";
        }
}

sub Error
{
  print STDERR "@_\n";
  close(SOCK);
  exit 0;
}
