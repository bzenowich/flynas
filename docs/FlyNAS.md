**FlyNAS**

Similar to FreeNAS/TrueNAS, but based on DragonFlyBSD. Uses HAMMER2 filesystem for data integrity, snapshots. Securely encrypted backup via cryfs-batch (separate project) to S3 bucket. Easy to install supported apps as VMs, or configure your own custom VM. Built-in Lua service monitoring (Uptime Kuma style, no Node dependency). Backend mostly written in Lua. Prefer simplicity to complexity.

Frontend UI layout

* https://github.com/nicbarker/clay  
* Renderer: web  
* Use TrueNAS UI as a style reference

Backend

* OpenResty (nginx and luajit)  
* SQLite (via lsqlite3)

Services

* Dashboard  
  * System info  
  * CPU usage  
  * Memory usage  
  * Network info  
  * Disk status (size, health, read iops, write iops)  
  * Pool status (health, %used by each VM, %free)  
  * Service monitor summary
* Network  
  * Select time zone, choose NTP server  
  * Static IP (IP, netmask, gateway) / DHCP (show MAC)  
* Accounts  
  * Users  
    * Username  
    * Public key  
      * Option to paste a public key  
      * Option to create ed25519 public/private keypair, store the public key, download the AES128 password-encrypted private key  
    * SSH access on/off  
      * Public-key only, no password auth  
      * Create home directory  
      * Choose shell  
  * Groups  
    * Synchronized with seafile tags  
* Storage (HAMMER2 multi-volume)
  * Create volume on disk
  * Add disk to volume (hammer2 volume-add)
  * Remove disk from volume (hammer2 volume-del)
  * Disk health monitoring (SMART)
  * Periodic scrub (daily cron job)  
* Backup  
  * Periodic snapshots  
    * Hourly for a day  
    * Daily for a week  
    * Weekly for a year  
    * Yearly  
    * Hourly cron job deletes expired snapshots  
  * S3 mount with rclone  
    * Add S3 bucket credentials  
    * Edit S3 bucket credentials  
    * Remove S3 bucket credentials  
  * cryfs-batch encrypted sync of snapshot data  
  * Restore: Create temporary seafile share from snapshot  
  * Import / Export FlyNAS config  
    * JSON format  
* Virtual machines (NVMM)  
  * Add VM  
    * ISO file  
    * CPUs  
    * RAM  
    * Storage size  
    * Static IP (IP, netmask, gateway) / DHCP (generate MAC)  
    * Post-installation config script  
    * Monitor config for this service  
  * Remove VM  
  * Start VM  
  * Suspend VM  
  * Stop VM  
* Applications (install as VMs)  
  * Seafile (file sharing)  
  * CryptPad (document collaboration)  
  * Forgejo (git collaboration server)  
  * VaultWarden (password manager)  
  * Readeck (browser bookmarks)  
  * Jellyfin (media server)  
  * RoundCube / smtp2go (email)  
  * Dokuwiki (wiki)  
  * Wekan (Kanban board)

