This docker folder contains the `docker-compose.yml` file for starting and configuring Couchbase Server and Sync Gateways for running non-walrus replicator tests with Sync Gateway.

When running `docker compose up`, there will be one Couchbase Server and two Sync Gateways started as follows:

### 1. Sync Gateway for .SyncServerCollection Tests

 | Config      | Value       |
 | ----------- | ----------- |
 | Database    | scratch     |
 | Port        | 4984        |
 | Admin Port  | 4985        |
 | Collections | flowers.roses, flowers.tulips, and flowers.lavenders |

 #### Sync Function
 ```JS
    function (doc, oldDoc) { 
        if (doc.isRejected == 'true') 
            throw({'forbidden':'read_only'}); 
        channel(doc.channels); 
    }
 ``` 

### 2. Legacy Sync Gateway for .SyncServer30 Tests

 | Config      | Value       |
 | ----------- | ----------- |
 | Database    | scratch-30  |
 | Port        | 4884        |
 | Admin Port  | 4885        |

 ** SG 3.0 doesn't support collection.

 #### Sync Function

 Use default sync function which is as follows
 ```JS
    function (doc, oldDoc) {
        channel(doc.channels); 
    }
 ``` 

The Admin Credentials of both Sync Gateways are `admin/password` or `Administrator/password`.

### Docker Compose Environment Variables

The `docker-compose.yml` has 3 environment variables for configuration.

 | Variable      |   Description  |
 | ------------- | -------------- |
 | SG_DEB        | Sync Gateway 3.1+ deb file URL. Default is SG 3.1.0 Ubuntu ARM64 URL.      |
 | SG_LEGACY_DEB | Sync Gateway 3.0 deb file URL. Default is SG 3.0.7 Ubuntu ARM64 URL.       |
 | SSL           | Boolean flag to configure Sync Gateway for SSL endpoints. Default is true. |

 Note: If you are using Mac x86-64, you must configure SG_DEB and SG_LEGACY_DEB to use x86_64 versions. 

 To configure environment variables, create `.env` file with the variables in key=value format.

 **Sample .env file for Mac x86-64**
```
SG_DEB=https://packages.couchbase.com/releases/couchbase-sync-gateway/3.1.0/couchbase-sync-gateway-enterprise_3.1.0_x86_64.deb
SG_LEGACY_DEB=https://packages.couchbase.com/releases/couchbase-sync-gateway/3.0.7/couchbase-sync-gateway-enterprise_3.0.7_x86_64.deb
```

### Some Commands

 |      Commands       |   Description  |
 | ------------------- | -------------- |
 | docker compose up   | Start all docker containers as specified in the `docker-compose.yml`. If the docker images and containers do not exists, they will be built first |
 | docker compose down | Stop running docker containers without deleting the containers |
 | docker compose down --rmi all | Stop running docker containers and deleting the images and containers |