class BlockDevice {
public:
    virtual bool read(unsigned lba, unsigned count, void* buffer) = 0;
    virtual bool write(unsigned lba, unsigned count, const void* buffer) = 0;
    virtual unsigned sector_count() = 0;
};
