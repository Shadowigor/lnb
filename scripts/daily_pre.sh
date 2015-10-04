#!/bin/bash

cd "$1"
source raw/.meta/daily.conf

let day+=1
next_backup=daily.$day

if [[ $day -gt 6 ]]; then
    day=0
    let week+=1
    next_backup=weekly.$week
    if [[ $week -gt 3 ]]; then
        week=0
        let month+=1
        next_backup=monthly.$month
        if [[ $month -gt 11 ]]; then
            month=0
            let year+=1
            next_backup=yearly.$year
        fi
    fi
fi

echo "$last_backup $next_backup"
