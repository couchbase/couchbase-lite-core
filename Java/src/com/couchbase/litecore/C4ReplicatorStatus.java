/**
 * Copyright (c) 2017 Couchbase, Inc. All rights reserved.
 * <p>
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 * <p>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p>
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

package com.couchbase.litecore;


public class C4ReplicatorStatus {
    public interface C4ReplicatorActivityLevel {
        int kC4Stopped = 0;
        int kC4Offline = 1;
        int kC4Connecting = 2;
        int kC4Idle = 3;
        int kC4Busy = 4;
    }

    private int activityLevel = -1;     // C4ReplicatorActivityLevel
    private long progressCompleted = 0L; // C4Progress.completed
    private long progressTotal = 0L;     // C4Progress.total
    private int errorDomain = 0;        // C4Error.domain
    private int errorCode = 0;          // C4Error.code
    private int errorInternalInfo = 0;  // C4Error.internal_info

    public C4ReplicatorStatus() {
    }

    public C4ReplicatorStatus(int activityLevel, long progressCompleted, long progressTotal, int errorDomain, int errorCode, int errorInternalInfo) {
        this.activityLevel = activityLevel;
        this.progressCompleted = progressCompleted;
        this.progressTotal = progressTotal;
        this.errorDomain = errorDomain;
        this.errorCode = errorCode;
        this.errorInternalInfo = errorInternalInfo;
    }

    public int getActivityLevel() {
        return activityLevel;
    }

    public void setActivityLevel(int activityLevel) {
        this.activityLevel = activityLevel;
    }

    public long getProgressCompleted() {
        return progressCompleted;
    }

    public long getProgressTotal() {
        return progressTotal;
    }

    public int getErrorDomain() {
        return errorDomain;
    }

    public int getErrorCode() {
        return errorCode;
    }

    public int getErrorInternalInfo() {
        return errorInternalInfo;
    }

    public C4Error getC4Error() {
        return new C4Error(errorDomain, errorCode, errorInternalInfo);
    }

    @Override
    public String toString() {
        return "C4ReplicatorStatus{" +
                "activityLevel=" + activityLevel +
                ", progressCompleted=" + progressCompleted +
                ", progressTotal=" + progressTotal +
                ", errorDomain=" + errorDomain +
                ", errorCode=" + errorCode +
                ", errorInternalInfo=" + errorInternalInfo +
                '}';
    }

    public C4ReplicatorStatus copy() {
        return new C4ReplicatorStatus(activityLevel, progressCompleted, progressTotal, errorDomain, errorCode, errorInternalInfo);
    }
}
