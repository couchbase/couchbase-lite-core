//
//  PerfTimer.cs
//
//  Author:
//   Jim Borden  <jim.borden@couchbase.com>
//
//  Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;

namespace LiteCore.Util
{
    public static class PerfTimer
    {
        #region Constants

        private static readonly ConcurrentDictionary<string, LinkedList<PerfEvent>> _EventMap = 
            new ConcurrentDictionary<string, LinkedList<PerfEvent>>();

        #endregion

        #region Public Methods

        [Conditional("PERF_TESTING")]
        public static void StartEvent(string name)
        {
            var evt = new PerfEvent(name);
            var list = _EventMap.GetOrAdd(name, new LinkedList<PerfEvent>());
            list.AddLast(evt);
            evt.StartTiming();
        }

        [Conditional("PERF_TESTING")]
        public static void StopEvent(string name)
        {
            var list = _EventMap[name];
            var evt = list.Last.Value;
            evt.StopTiming();
        }

        [Conditional("PERF_TESTING")]
        public static void WriteStats(Action<string> handler)
        {
            if(handler == null) {
                return;
            }

            var summaryDict = new SortedDictionary<double, string>();
            foreach(var pair in _EventMap) {
                var average = pair.Value.Average(x => x.Elapsed.TotalMilliseconds);
                summaryDict[average] = pair.Key;
            }

            var unitMap = new List<string> { "ms", "μs", "ns" };
            foreach(var pair in summaryDict.Reverse()) {
                var time = pair.Key;
                var index = 0;
                while(index < 3 && time < 1) {
                    index++;
                    time *= 1000.0;
                }
                handler($"{pair.Value} => Average {time}{unitMap[index]}");
            }
        }

        #endregion
    }
    internal sealed class PerfEvent
    {
        #region Variables

        private Stopwatch _sw;

        #endregion

        #region Properties

        internal TimeSpan Elapsed
        {
            get {
                return _sw.Elapsed;
            }
        }

        internal string Name { get; }

        #endregion

        #region Constructors

        internal PerfEvent(string name)
        {
            Name = name;
        }

        #endregion

        #region Internal Methods

        internal void StartTiming()
        {
            _sw = Stopwatch.StartNew();
        }

        internal void StopTiming()
        {
            _sw.Stop();
        }

        #endregion
    }
}
