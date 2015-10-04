#!/bin/bash

function move
{
    if [[ -e "$1$2" ]]; then
        rm "$1$2"
    fi

    i=$2
    while [[ $i -gt 0 ]]; do
        let i-=1
        if [[ -e "$1$i" ]]; then
            mv "$1$i" "$1$(expr $i + 1)"
        fi
    done
}

cd "$1"
source .meta/daily.conf
link_backups="daily.0"

let day+=1
next_backup=daily.$day

move "../daily." 6
if [[ $day -gt 6 ]]; then
    day=0
    let week+=1
    move "../weekly." 3
    next_backup=weekly.$week
    link_backups="$link_backups weekly.0"
    if [[ $week -gt 3 ]]; then
        week=0
        let month+=1
        move "../monthly." 11
        next_backup=monthly.$month
        link_backups="$link_backups monthly.0"
        if [[ $month -gt 11 ]]; then
            month=0
            let year+=1
            move "../yearly." 3
            next_backup=yearly.$year
            link_backups="$link_backups yearly.0"
        fi
    fi
fi

echo "last_backup=$next_backup" > .meta/daily.conf
echo "year=$year" >> .meta/daily.conf
echo "month=$month" >> .meta/daily.conf
echo "week=$week" >> .meta/daily.conf
echo "day=$day" >> .meta/daily.conf

for link_name in $link_backups; do
    ln -rs "$next_backup" "../$link_name"
done
